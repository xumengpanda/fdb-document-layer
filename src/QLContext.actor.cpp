/*
 * QLContext.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ExtStructs.h"
#include "ExtUtil.actor.h"
#include "QLContext.h"
#include "QLExpression.h"
#include "QLProjection.h"
#include "QLTypes.h"

#include "DocumentError.h"

using namespace FDB;

Future<DataValue> IReadContext::toDataValue() {
	return getRecursiveKnownPresent(Reference<IReadContext>::addRef(this));
}

ACTOR Future<Void> DocTransaction::commitChanges(Reference<DocTransaction> self, std::string docPrefix) {
	auto info = self->infos.find(docPrefix);
	if (info == self->infos.end())
		return Void();
	Void _ = wait(info->second->commitChanges(self));
	return Void();
}

ACTOR Future<Void> DocumentDeferred::commitChanges(Reference<DocTransaction> tr, Reference<DocumentDeferred> self) {
	Void _ = wait(self->snapshotLock.onUnused());
	for (auto& f : self->deferred)
		f(tr);
	self->writes_finished.send(Void());
	Void _ = wait(waitForAll(self->index_update_actors));
	self->writes_finished = Promise<Void>();
	return Void();
}

void DocTransaction::cancel_ongoing_index_reads() {
	for (auto it : infos) {
		for (auto actor : it.second->index_update_actors)
			actor.cancel();
	}
}

struct ITDoc {
	virtual void addref() = 0;
	virtual void delref() = 0;

	virtual Future<Optional<DataValue>> get(Reference<DocTransaction> tr, DataKey key) { return next->get(tr, key); }
	virtual GenFutureStream<KeyValue> getDescendants(Reference<DocTransaction> tr,
	                                                 DataKey key,
	                                                 StringRef begin,
	                                                 StringRef end,
	                                                 Reference<FlowLockHolder> flowControlLock) {
		return next->getDescendants(tr, key, begin, end, flowControlLock);
	}

	virtual void set(Reference<DocTransaction> tr, DataKey key, ValueRef value) { return next->set(tr, key, value); }

	virtual void clearDescendants(Reference<DocTransaction> tr, DataKey key) { return next->clearDescendants(tr, key); }

	virtual void clear(Reference<DocTransaction> tr, DataKey key) { return next->clear(tr, key); }

	virtual std::string toString() { return "unimplemented"; }

protected:
	Reference<ITDoc> next;
	explicit ITDoc(Reference<ITDoc> next) : next(next) {}
	ITDoc() : next(nullptr) {}
	~ITDoc() = default;
};

/*static std::string getFDBKey( DataKey const& key, int extraReserveBytes = 0 ) {
    std::string r;
    r.reserve(1 + key.byteSize() + extraReserveBytes);
    r.assign(1, (char)0xbd);
    r += key.toString();
    return r;
}*/

static std::string getFDBKey(DataKey const& key, int extraReserveBytes = 0) {
	return key.toString();
}

ACTOR static Future<Optional<DataValue>> FDBPlugin_get(DataKey key, Reference<DocTransaction> tr) {
	Optional<FDBStandalone<ValueRef>> v = wait(tr->tr->get(KeyRef(getFDBKey(key))));
	if (v.present()) {
		return Optional<DataValue>(DataValue::decode_value(v.get()));
	} else {
		return Optional<DataValue>();
	}
}

static inline void operator+=(std::string& lhs, StringRef const& rhs) {
	lhs.append((const char*)rhs.begin(), rhs.size());
}

static inline std::string strAppend(std::string& lhs, StringRef const& rhs) {
	std::string r;
	r.reserve(lhs.size() + rhs.size());
	r = lhs;
	r += rhs;
	return r;
}

ACTOR static Future<Void> FDBPlugin_getDescendants(DataKey key,
                                                   Reference<DocTransaction> tr,
                                                   Standalone<StringRef> relBegin,
                                                   Standalone<StringRef> relEnd,
                                                   PromiseStream<KeyValue> output,
                                                   Reference<FlowLockHolder> flowControlLock) {
	std::string prefix = getFDBKey(key, relEnd.size());
	state int substrOffset = static_cast<int>(prefix.size());
	state std::string begin = strAppend(prefix, relBegin);
	state std::string end = std::move(prefix);
	end += relEnd;

	// if (verboseLogging)
	// TraceEvent("BD_getDescendents").detail("from", printable(StringRef(begin)).c_str()).detail("to",
	// printable(StringRef(end)).c_str());

	try {
		state GetRangeLimits limit(GetRangeLimits::ROW_LIMIT_UNLIMITED, 80000);
		state Future<FDBStandalone<RangeResultRef>> nextRead = tr->tr->getRange(KeyRangeRef(begin, end), limit);
		loop {
			state FDBStandalone<RangeResultRef> rr = wait(nextRead);

			if (rr.more) {
				begin = keyAfter(rr.back().key).toString();
				nextRead = tr->tr->getRange(KeyRangeRef(begin, end), limit);
			}

			while (!rr.empty()) {
				state int permits = rr.size();
				if (flowControlLock)
					Void _ = wait(flowControlLock->lock->takeUpTo(permits));

				for (int i = 0; i < permits; i++) {
					auto& kv = rr[i];
					output.send(KeyValue(KeyValueRef(kv.key.substr(substrOffset), kv.value)));
				}
				static_cast<VectorRef<KeyValueRef>&>(rr) = rr.slice(permits, rr.size());
			}

			if (!rr.more)
				break;
		}

		throw end_of_stream();
	} catch (Error& e) {
		if (e.code() != error_code_end_of_stream && e.code() != error_code_operation_cancelled)
			TraceEvent(SevError, "BD_getDescendants").detail("error", e.what());
		if (e.code() != error_code_operation_cancelled)
			output.sendError(e);
		throw;
	}
}

struct FDBPlugin : ITDoc, ReferenceCounted<FDBPlugin>, FastAllocated<FDBPlugin> {
	void addref() override { ReferenceCounted<FDBPlugin>::addref(); }
	void delref() override { ReferenceCounted<FDBPlugin>::delref(); }

	std::pair<bool, Reference<DocumentDeferred>> findOrCreate(Reference<DocTransaction> tr, DataKey const& key) {
		if (key.size() > 1) {
			std::string documentPrefix = key.keyPrefix(2).toString();
			auto info = tr->infos.find(documentPrefix);
			if (info == tr->infos.end())
				info = tr->infos
				           .insert(std::make_pair(documentPrefix, Reference<DocumentDeferred>(new DocumentDeferred())))
				           .first;
			return std::make_pair(true, (*info).second);
		}
		return std::make_pair(false, Reference<DocumentDeferred>());
	}

	Future<Optional<DataValue>> get(Reference<DocTransaction> tr, DataKey key) override {
		return FDBPlugin_get(key, tr);
	}
	GenFutureStream<KeyValue> getDescendants(Reference<DocTransaction> tr,
	                                         DataKey key,
	                                         StringRef begin,
	                                         StringRef end,
	                                         Reference<FlowLockHolder> flowControlLock) override {
		PromiseStream<KeyValue> p;
		GenFutureStream<KeyValue> r(p.getFuture());
		r.actor = FDBPlugin_getDescendants(key, tr, begin, end, p, flowControlLock);
		return r;
	}
	void set(Reference<DocTransaction> tr, DataKey key, ValueRef value) override {
		std::string k = getFDBKey(key);
		Value v = value;
		auto pair = findOrCreate(tr, key);
		if (pair.first)
			pair.second->deferred.emplace_back([k, v](Reference<DocTransaction> tr) {
				tr->tr->set(k, v);
				return Void();
			});
	}
	void clearDescendants(Reference<DocTransaction> tr, DataKey key) override {
		std::string _key = getFDBKey(key);

		KeyRange kr = KeyRangeRef(_key + '\x00', _key + '\xFF');
		auto pair = findOrCreate(tr, key);
		if (pair.first)
			pair.second->deferred.emplace_back([kr](Reference<DocTransaction> tr) {
				tr->tr->clear(kr);
				return Void();
			});
	}
	void clear(Reference<DocTransaction> tr, DataKey key) override {
		std::string k = getFDBKey(key);
		auto pair = findOrCreate(tr, key);
		if (pair.first)
			pair.second->deferred.emplace_back([k](Reference<DocTransaction> tr) {
				tr->tr->clear(k);
				return Void();
			});
	}
	std::string toString() override { return "FDBPlugin"; }
};

struct IndexPlugin : ITDoc {
	virtual Future<Void> doIndexUpdate(Reference<DocTransaction> tr,
	                                   Reference<DocumentDeferred> dd,
	                                   DataKey documentPath) = 0;

	std::pair<bool, Reference<DocumentDeferred>> shouldDoUpdate(Reference<DocTransaction> tr,
	                                                            DataKey const& documentKey) {
		if (!error_state && documentKey.startsWith(collectionPath) && documentKey.size() > collectionPath.size()) {
			std::string documentPrefix = documentKey.toString();
			auto info = tr->infos.find(documentPrefix);
			if (info == tr->infos.end())
				info = tr->infos
				           .insert(std::make_pair(documentPrefix, Reference<DocumentDeferred>(new DocumentDeferred())))
				           .first;
			return std::make_pair(true, (*info).second);
		}
		return std::make_pair(false, Reference<DocumentDeferred>());
	}

	void set(Reference<DocTransaction> tr, DataKey key, ValueRef value) override {
		DataKey documentPrefix = key.keyPrefix(collectionPath.size() + 1);
		auto pair = shouldDoUpdate(tr, documentPrefix);
		if (pair.first && pair.second->dirty.insert(this).second) {
			pair.second->index_update_actors.push_back(doIndexUpdate(tr, pair.second, documentPrefix));
		}

		next->set(tr, key, value);
	}

	void clear(Reference<DocTransaction> tr, DataKey key) override {
		DataKey documentPrefix = key.keyPrefix(collectionPath.size() + 1);
		auto pair = shouldDoUpdate(tr, documentPrefix);
		if (pair.first && pair.second->dirty.insert(this).second) {
			pair.second->index_update_actors.push_back(doIndexUpdate(tr, pair.second, documentPrefix));
		}

		next->clear(tr, key);
	}

	void clearDescendants(Reference<DocTransaction> tr, DataKey key) override {
		DataKey documentPrefix = key.keyPrefix(collectionPath.size() + 1);
		auto pair = shouldDoUpdate(tr, documentPrefix);
		if (pair.first && pair.second->dirty.insert(this).second) {
			pair.second->index_update_actors.push_back(doIndexUpdate(tr, pair.second, documentPrefix));
		}
		next->clearDescendants(tr, key);
	}

	DataKey collectionPath;
	DataKey indexPath;
	bool error_state;
	bool multikey;
	bool isUniqueIndex;
	Optional<Reference<FlowLockHolder>> flowControlLock;

	IndexPlugin(DataKey collectionPath, IndexInfo indexInfo, Reference<ITDoc> next)
	    : collectionPath(collectionPath),
	      indexPath(indexInfo.indexCx->getPrefix()), // dbName+collectionName+"metadata"+"indices"+indexName
	      ITDoc(next),
	      error_state(false),
	      multikey(indexInfo.multikey),
	      isUniqueIndex(indexInfo.isUniqueIndex),
	      flowControlLock(indexInfo.isUniqueIndex ? Optional<Reference<FlowLockHolder>>(
	                                                    Reference<FlowLockHolder>(new FlowLockHolder(new FlowLock(1))))
	                                              : Optional<Reference<FlowLockHolder>>()) {}
};

struct CompoundIndexPlugin : IndexPlugin, ReferenceCounted<CompoundIndexPlugin>, FastAllocated<CompoundIndexPlugin> {
	void addref() override { ReferenceCounted<CompoundIndexPlugin>::addref(); }
	void delref() override { ReferenceCounted<CompoundIndexPlugin>::delref(); }

	ACTOR static Future<Void> doIndexUpdateActor(Reference<CompoundIndexPlugin> self,
	                                             Reference<DocTransaction> tr,
	                                             Reference<DocumentDeferred> dd,
	                                             DataKey documentPath) {
		state Reference<QueryContext> doc(new QueryContext(self->next, tr, documentPath));
		state Future<Void> writes_finished = dd->writes_finished.getFuture();

		try {
			// TraceEvent("BD_doIndexUpdateStart");

			dd->snapshotLock.use();

			std::vector<Future<std::vector<DataValue>>> f_old_values;
			for (const auto& expr : self->exprs) {
				f_old_values.push_back(
				    consumeAll(mapAsync(expr.first->evaluate(doc), [](Reference<IReadContext> valcx) {
					    return getMaybeRecursive(valcx, StringRef());
				    })));
			}

			state std::vector<std::vector<DataValue>> old_values = wait(getAll(f_old_values));

			dd->snapshotLock.unuse();

			Void _ = wait(writes_finished);

			std::vector<Future<std::vector<DataValue>>> f_new_values;
			for (const auto& expr : self->exprs) {
				f_new_values.push_back(
				    consumeAll(mapAsync(expr.first->evaluate(doc), [](Reference<IReadContext> valcx) {
					    return getMaybeRecursive(valcx, StringRef());
				    })));
			}

			state std::vector<std::vector<DataValue>> new_values = wait(getAll(f_new_values));

			int num_new_values = 1;
			for (const auto& v : new_values) {
				num_new_values *= v.size();
			}

			if (num_new_values > DOCLAYER_KNOBS->MULTI_MULTIKEY_INDEX_MAX) {
				self->error_state = true;
				throw multikey_index_cartesian_explosion();
			}

			state cartesian_product_iterator<DataValue, std::vector<DataValue>::iterator> nvv(new_values);
			if (self->isUniqueIndex) {
				// for all new entries going to be written, before we clear the potentially existing old index entries,
				// we need to make sure there is no existing unique index for that new value under path
				// dbName+collectionName+"metadata"+"indices"+encodedIndexName+encodedValue
				if (self->flowControlLock.present()) {
					// When building the unique index, each record out of the table scan needs to wait for the previous
					// one finished duplication detecting and index record writing. And thus the following section
					// before the `lock.release()` call, needs to be protected using a mutex lock.
					Void _ = wait(self->flowControlLock.get()->lock->take(1));
				}
				for (; nvv; ++nvv) {
					DataKey potential_index_key(self->indexPath);
					for (int i = 0; i < nvv.size(); i++)
						potential_index_key.append(nvv[i].encode_key_part());
					std::vector<Standalone<FDB::KeyValueRef>> existing_index_entries =
					    wait(consumeAll(self->getDescendants(tr, potential_index_key, LiteralStringRef("\x00"),
					                                         LiteralStringRef("\xff"), Reference<FlowLockHolder>())));
					// There are two major scenarios that may cause the violation:
					//    1. During the building of unique index:
					//      In this case, the existing entry will NEVER point to the same docId. So we simply throw
					//      whenever an existing entry found for this value.
					//    2. During the insert of a new value or update an existing value:
					//      Since the unique index was successfully built previously, there will be at most one
					//      existing index for this value.
					//         - If that entry has the same docId, we are fine because
					//            this could be an update which is actually a no-op.
					//         - Or, it has a different docId, that's when we throw.
					// Thus we just compare the docId if there is an existing entry, although it's redundant
					// for case #1.
					if (!existing_index_entries.empty()) {
						Standalone<StringRef> existingDocId(
						    DataKey::decode_item_rev(existing_index_entries.front().key, 0),
						    existing_index_entries.front().arena());
						if (existingDocId.compare(documentPath[documentPath.size() - 1])) {
							// existing index points to a different doc id that has the same value, abort.
							throw duplicated_key_field();
						}
					}
				}
			}
			// clear all existing index entries
			for (cartesian_product_iterator<DataValue, std::vector<DataValue>::iterator> ovv(old_values); ovv; ++ovv) {
				// fprintf(stderr, "Old value: %s\n", printable(StringRef(v.encode_key_part())).c_str());
				DataKey old_key(self->indexPath);
				for (int i = 0; i < ovv.size(); i++)
					old_key.append(ovv[i].encode_key_part());
				old_key.append(documentPath[documentPath.size() - 1]);
				tr->tr->clear(getFDBKey(old_key));
			}
			// write the new/updated index entries
			nvv.reset();
			for (; nvv; ++nvv) {
				// fprintf(stderr, "New value: %s\n", printable(StringRef(v.encode_key_part())).c_str());
				DataKey new_key(self->indexPath);
				for (int i = 0; i < nvv.size(); i++)
					new_key.append(nvv[i].encode_key_part());
				new_key.append(documentPath[documentPath.size() - 1]);
				tr->tr->set(getFDBKey(new_key), StringRef());
			}

			if (self->flowControlLock.present()) {
				self->flowControlLock.get()->lock->release(1);
			}

		} catch (Error& e) {
			TraceEvent(SevError, "BD_doIndexUpdate").detail("error", e.what());
			throw;
		}

		return Void();
	}

	Future<Void> doIndexUpdate(Reference<DocTransaction> tr,
	                           Reference<DocumentDeferred> dd,
	                           DataKey documentPath) override {
		return doIndexUpdateActor(Reference<CompoundIndexPlugin>::addRef(this), tr, dd, documentPath);
	}

	std::string toString() override { return "CompoundIndexPlugin"; }

	CompoundIndexPlugin(DataKey collectionPath,
	                    IndexInfo indexInfo,
	                    std::vector<std::pair<Reference<IExpression>, int>> exprs,
	                    Reference<ITDoc> next)
	    : IndexPlugin(collectionPath, indexInfo, next), exprs(exprs) {}

	std::vector<std::pair<Reference<IExpression>, int>> exprs;
};

struct SimpleIndexPlugin : IndexPlugin, ReferenceCounted<SimpleIndexPlugin>, FastAllocated<SimpleIndexPlugin> {
	void addref() override { ReferenceCounted<SimpleIndexPlugin>::addref(); }
	void delref() override { ReferenceCounted<SimpleIndexPlugin>::delref(); }

	// documentPath is ns + docId
	Future<Void> doIndexUpdate(Reference<DocTransaction> tr,
	                           Reference<DocumentDeferred> dd,
	                           DataKey documentPath) override {
		return doIndexUpdateActor(Reference<SimpleIndexPlugin>::addRef(this), tr, dd, documentPath);
	}

	ACTOR static Future<Void> doIndexUpdateActor(Reference<SimpleIndexPlugin> self,
	                                             Reference<DocTransaction> tr,
	                                             Reference<DocumentDeferred> dd,
	                                             DataKey documentPath) {
		state Reference<QueryContext> doc(new QueryContext(self->next, tr, documentPath));
		state Future<Void> writes_finished = dd->writes_finished.getFuture();
		try {
			// TraceEvent("BD_doIndexUpdateStart");

			dd->snapshotLock.use();

			state std::vector<DataValue> old_values =
			    wait(consumeAll(mapAsync(self->expr->evaluate(doc), [](Reference<IReadContext> valcx) {
				    return getMaybeRecursive(valcx, StringRef());
			    })));

			dd->snapshotLock.unuse();

			Void _ = wait(writes_finished);

			state std::vector<DataValue> new_values =
			    wait(consumeAll(mapAsync(self->expr->evaluate(doc), [](Reference<IReadContext> valcx) {
				    return getMaybeRecursive(valcx, StringRef());
			    })));
			if (self->isUniqueIndex) {
				// for all new entries going to be written, before we clear the potentially existing old index entries,
				// we need to make sure there is no existing unique index for that new value under path
				// dbName+collectionName+"metadata"+"indices"+encodedIndexName+encodedValue

				if (self->flowControlLock.present()) {
					// When building the unique index, each record out of the table scan needs to wait for the previous
					// one finished duplication detecting and index record writing. And thus the following section
					// before the `lock.release()` call, needs to be protected using a mutex lock.
					Void _ = wait(self->flowControlLock.get()->lock->take(1));
				}
				for (const DataValue& v : new_values) {
					state DataKey potential_index_key(self->indexPath);
					potential_index_key.append(v.encode_key_part());
					std::vector<Standalone<FDB::KeyValueRef>> existing_index_entries =
					    wait(consumeAll(self->getDescendants(tr, potential_index_key, LiteralStringRef("\x00"),
					                                         LiteralStringRef("\xff"), Reference<FlowLockHolder>())));
					// There are two major scenarios that may cause the violation:
					//    1. During the building of unique index:
					//      In this case, the existing entry will NEVER point to the same docId. So we simply throw
					//      whenever an existing entry found for this value.
					//    2. During the insert of a new value or update an existing value:
					//      Since the unique index was successfully built previously, there will be at most one
					//      existing index for this value.
					//         - If that entry has the same docId, we are fine because
					//            this could be an update which is actually a no-op.
					//         - Or, it has a different docId, that's when we throw.
					// Thus we just compare the docId if there is an existing entry, although it's redundant
					// for case #1.
					if (!existing_index_entries.empty()) {
						Standalone<StringRef> existingDocId(
						    DataKey::decode_item_rev(existing_index_entries.front().key, 0),
						    existing_index_entries.front().arena());
						if (existingDocId.compare(documentPath[documentPath.size() - 1])) {
							// existing index points to a different doc id that has the same value, abort.
							self->error_state = true;
							throw duplicated_key_field();
						}
					}
				}
			}
			// clear any existing index entries
			for (DataValue& v : old_values) {
				// fprintf(stderr, "Old value: %s\n", printable(StringRef(v.encode_key_part())).c_str());
				DataKey old_key(self->indexPath);
				old_key.append(v.encode_key_part()).append(documentPath[documentPath.size() - 1]);
				tr->tr->clear(getFDBKey(old_key));
			}
			// write the new/updated index entries
			for (DataValue& v : new_values) {
				// fprintf(stderr, "New value: %s\n", printable(StringRef(v.encode_key_part())).c_str());
				DataKey new_key(self->indexPath);
				new_key.append(v.encode_key_part()).append(documentPath[documentPath.size() - 1]);
				tr->tr->set(getFDBKey(new_key), StringRef());
			}
			if (self->flowControlLock.present()) {
				self->flowControlLock.get()->lock->release(1);
			}
		} catch (Error& e) {
			TraceEvent(SevError, "BD_doIndexUpdate").detail("error", e.what());
			throw;
		}

		return Void();
	}

	std::string toString() override { return "SimpleIndexPlugin"; }

	SimpleIndexPlugin(DataKey collectionPath, IndexInfo indexInfo, Reference<IExpression> expr, Reference<ITDoc> next)
	    : IndexPlugin(collectionPath, indexInfo, next), expr(expr) {}

	Reference<IExpression> expr;
};

struct QueryContextData {
	explicit QueryContextData(Reference<DocTransaction> tr) : tr(tr) { layers = Reference<ITDoc>(new FDBPlugin()); }

	QueryContextData(Reference<ITDoc> layers, Reference<DocTransaction> tr, DataKey prefix)
	    : layers(layers), tr(tr), prefix(prefix) {}

	QueryContextData(QueryContextData* const& other, StringRef sub)
	    : tr(other->tr), prefix(other->prefix), layers(other->layers) {
		prefix.append(sub);
	}

	virtual Future<Optional<DataValue>> get(StringRef key) { return layers->get(tr, DataKey(prefix).append(key)); }
	virtual GenFutureStream<KeyValue> getDescendants(StringRef begin,
	                                                 StringRef end,
	                                                 Reference<FlowLockHolder> flowControlLock) {
		return layers->getDescendants(tr, prefix, begin, end, flowControlLock);
	}
	virtual void set(StringRef key, ValueRef value) { layers->set(tr, DataKey(prefix).append(key), value); }
	virtual void clearDescendants() { layers->clearDescendants(tr, prefix); }
	virtual void clear(StringRef key) { layers->clear(tr, DataKey(prefix).append(key)); }
	virtual void clearRoot() { layers->clear(tr, prefix); }

	DataKey prefix;
	Reference<DocTransaction> tr;
	Reference<ITDoc> layers;
};

QueryContext::QueryContext(Reference<DocTransaction> tr) : self(new QueryContextData(tr)) {}

QueryContext* QueryContext::v_getSubContext(StringRef sub) {
	return new QueryContext(*this, sub);
}

QueryContext::QueryContext(QueryContext const& other, StringRef sub) : self(new QueryContextData(other.self, sub)) {}

QueryContext::QueryContext(class Reference<ITDoc> layers, Reference<DocTransaction> tr, DataKey path)
    : self(new QueryContextData(layers, tr, path)) {}

void QueryContext::addIndex(IndexInfo index) {
	if (index.indexKeys.size() == 1) {
		self->layers = Reference<ITDoc>(new SimpleIndexPlugin(
		    self->prefix, index,
		    Reference<IExpression>(new ExtPathExpression(StringRef(index.indexKeys[0].first), true, true)),
		    self->layers));
	} else {
		std::vector<std::pair<Reference<IExpression>, int>> exprs(index.indexKeys.size());
		std::transform(index.indexKeys.begin(), index.indexKeys.end(), exprs.begin(),
		               [](std::pair<std::string, int> index_pair) {
			               return std::make_pair(
			                   Reference<IExpression>(new ExtPathExpression(StringRef(index_pair.first), true, true)),
			                   index_pair.second);
		               });
		self->layers = Reference<ITDoc>(new CompoundIndexPlugin(self->prefix, index, exprs, self->layers));
	}
}

Future<Optional<DataValue>> QueryContext::get(StringRef key) {
	return self->get(key);
}

GenFutureStream<KeyValue> QueryContext::getDescendants(StringRef begin,
                                                       StringRef end,
                                                       Reference<FlowLockHolder> flowControlLock) {
	return self->getDescendants(begin, end, flowControlLock);
}

void QueryContext::set(StringRef key, ValueRef value) {
	return self->set(key, value);
}

void QueryContext::clearDescendants() {
	return self->clearDescendants();
}

void QueryContext::clear(StringRef key) {
	return self->clear(key);
}

void QueryContext::clearRoot() {
	return self->clearRoot();
}

Future<Void> QueryContext::commitChanges() {
	return self->tr->commitChanges(self->prefix.toString());
}

const DataKey QueryContext::getPrefix() {
	return self->prefix;
}

void QueryContext::printPlugins() {
	fprintf(stderr, "Top plugin: %s\n", self->layers->toString().c_str());
}

Reference<DocTransaction> QueryContext::getTransaction() {
	return self->tr;
}

QueryContext::~QueryContext() {
	delete self;
}

Reference<UnboundQueryContext> UnboundQueryContext::getSubContext(StringRef sub) {
	return Reference<UnboundQueryContext>(new UnboundQueryContext(*this, sub));
}

Reference<QueryContext> UnboundQueryContext::bindQueryContext(Reference<DocTransaction> tr) {
	return Reference<QueryContext>(new QueryContext(tr))->getSubContext(prefix.toString());
}

const DataKey UnboundQueryContext::getPrefix() {
	return prefix;
}

Reference<CollectionContext> UnboundCollectionContext::bindCollectionContext(Reference<DocTransaction> tr) {
	return Reference<CollectionContext>(new CollectionContext(tr, Reference<UnboundCollectionContext>::addRef(this)));
}

void UnboundCollectionContext::addIndex(IndexInfo info) {
	knownIndexes.push_back(info);
	if (info.status == IndexInfo::IndexStatus::READY) {
		auto encodedFirstFieldname = DataValue(info.indexKeys[0].first, DVTypeCode::STRING).encode_key_part();
		auto sim_iterator = simpleIndexMap.find(encodedFirstFieldname);
		if (sim_iterator == simpleIndexMap.end()) {
			std::set<IndexInfo, index_compare> iSet;
			iSet.insert(info);
			simpleIndexMap.insert(make_pair(encodedFirstFieldname, iSet));
		} else {
			sim_iterator->second.insert(info);
		}
	}
}

Key UnboundCollectionContext::getIndexesSubspace() {
	return Standalone<StringRef>(metadataDirectory->key().toString() +
	                             DataValue(std::string("indices")).encode_key_part());
}

Reference<UnboundQueryContext> UnboundCollectionContext::getIndexesContext() {
	return Reference<UnboundQueryContext>(new UnboundQueryContext(DataKey()))->getSubContext(getIndexesSubspace());
}

Optional<IndexInfo> UnboundCollectionContext::getSimpleIndex(StringRef simple_index_map_key) {
	if (bannedFieldNames.present() &&
	    bannedFieldNames.get().find(DataValue::decode_key_part(simple_index_map_key).getString()) !=
	        bannedFieldNames.get().end())
		return Optional<IndexInfo>();
	auto index = simpleIndexMap.find(simple_index_map_key.toString());
	if (index == simpleIndexMap.end()) {
		return Optional<IndexInfo>();
	} else {
		return *index->second.begin();
	}
}

Optional<IndexInfo> UnboundCollectionContext::getCompoundIndex(IndexInfo prefix, StringRef encoded_next_index_key) {
	if (bannedFieldNames.present() &&
	    bannedFieldNames.get().find(DataValue::decode_key_part(encoded_next_index_key).getString()) !=
	        bannedFieldNames.get().end())
		return Optional<IndexInfo>();
	auto indexV = simpleIndexMap.find(DataValue(prefix.indexKeys[0].first, DVTypeCode::STRING).encode_key_part());
	ASSERT(indexV != simpleIndexMap.end());
	for (IndexInfo index : indexV->second) {
		if (index.size() > prefix.size() && index.hasPrefix(prefix)) {
			if (DataValue(index.indexKeys[prefix.indexKeys.size()].first, DVTypeCode::STRING).encode_key_part() ==
			    encoded_next_index_key) {
				return index;
			}
		}
	}
	return Optional<IndexInfo>();
}

Key UnboundCollectionContext::getVersionKey() {
	return Key(
	    KeyRef(metadataDirectory->key().toString() + DataValue("version", DVTypeCode::STRING).encode_key_part()));
}

std::string UnboundCollectionContext::databaseName() {
	return collectionDirectory->getPath()[1].toString();
}

std::string UnboundCollectionContext::collectionName() {
	return collectionDirectory->getPath().back().toString();
}

void CollectionContext::bumpMetadataVersion() {
	cx->getTransaction()->tr->atomicOp(unbound->getVersionKey(), LiteralStringRef("\x01\x00\x00\x00\x00\x00\x00\x00"),
	                                   FDB_MUTATION_TYPE_ADD);
}

Future<uint64_t> CollectionContext::getMetadataVersion() {
	Future<Optional<FDBStandalone<StringRef>>> fov = cx->getTransaction()->tr->get(
	    StringRef(unbound->getVersionKey())); // FIXME: Wow how many abstractions does this violate at once?
	Future<uint64_t> ret = map(fov, [](Optional<FDBStandalone<StringRef>> ov) -> uint64_t {
		if (!ov.present())
			return 0;
		else
			return *((uint64_t*)(ov.get().begin()));
	});
	return ret;
}

Future<Standalone<StringRef>> IReadWriteContext::getValueEncodedId() {
	return map(getMaybeRecursiveIfPresent(getSubContext(DataValue("_id", DVTypeCode::STRING).encode_key_part())),
	           [](Optional<DataValue> odv) -> Standalone<StringRef> {
		           return odv.present() ? odv.get().encode_value() : StringRef();
	           }); // FIXME: this is inefficient in about 12 different ways
}

Future<Standalone<StringRef>> IReadWriteContext::getKeyEncodedId() {
	return map(getMaybeRecursiveIfPresent(getSubContext(DataValue("_id", DVTypeCode::STRING).encode_key_part())),
	           [](Optional<DataValue> odv) -> Standalone<StringRef> {
		           return odv.present() ? odv.get().encode_key_part() : StringRef();
	           }); // FIXME: this is inefficient in about 12 different ways
}

Future<Standalone<StringRef>> BsonContext::getValueEncodedId() {
	bson::BSONElement e;
	bool okay = obj.getObjectID(e);
	return Future<Standalone<StringRef>>(okay ? DataValue(e).encode_value() : StringRef());
}

Future<Standalone<StringRef>> BsonContext::getKeyEncodedId() {
	bson::BSONElement e;
	bool okay = obj.getObjectID(e);
	return Future<Standalone<StringRef>>(okay ? DataValue(e).encode_key_part() : StringRef());
}

IndexInfo::IndexInfo(std::string indexName,
                     std::vector<std::pair<std::string, int>> indexKeys,
                     Reference<UnboundCollectionContext> collectionCx,
                     IndexStatus status,
                     Optional<UID> buildId,
                     bool isUniqueIndex)
    : indexName(indexName), indexKeys(indexKeys), status(status), buildId(buildId), isUniqueIndex(isUniqueIndex) {
	encodedIndexName = DataValue(indexName, DVTypeCode::STRING).encode_key_part();
	indexCx = collectionCx->getIndexesContext()->getSubContext(encodedIndexName);
	multikey = true;
}

bool IndexInfo::hasPrefix(IndexInfo const& other) {
	for (int i = 0; i < other.size(); i++) {
		if (indexKeys[i] != other.indexKeys[i]) {
			return false;
		}
	}
	return true;
}
