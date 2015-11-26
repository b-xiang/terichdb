#include "mock_db_engine.hpp"
#include <nark/io/FileStream.hpp>
#include <nark/io/StreamBuffer.hpp>
#include <nark/io/DataIO.hpp>
#include <boost/filesystem.hpp>

namespace nark { namespace db {

namespace fs = boost::filesystem;

llong MockReadonlyStore::dataStorageSize() const {
	return m_rows.used_mem_size();
}
llong MockReadonlyStore::numDataRows() const {
	return m_rows.size();
}
void
MockReadonlyStore::getValueAppend(llong id, valvec<byte>* val, DbContext*)
const {
	assert(id >= 0);
	if (m_fixedLen) {
		assert(0 == llong(m_rows.strpool.size() % m_fixedLen));
		assert(id < llong(m_rows.strpool.size() / m_fixedLen));
		val->append(m_rows.strpool.data() + m_fixedLen * id, m_fixedLen);
	} else {
		assert(id < llong(m_rows.size()));
		val->append(m_rows[id]);
	}
}
StoreIteratorPtr MockReadonlyStore::createStoreIter(DbContext*) const {
	assert(0); // should not be called
	return nullptr;
}

void MockReadonlyStore::build(SchemaPtr schema, SortableStrVec& data) {
	size_t fixlen = schema->getFixedRowLen();
	if (0 == fixlen) {
		if (data.str_size() >= UINT32_MAX) {
			THROW_STD(length_error,
				"keys.str_size=%lld is too large", llong(data.str_size()));
		}
		// reuse memory of keys.m_index
		auto offsets = (uint32_t*)data.m_index.data();
		size_t rows = data.m_index.size();
		for (size_t i = 0; i < rows; ++i) {
			uint32_t offset = uint32_t(data.m_index[i].offset);
			offsets[i] = offset;
		}
		offsets[rows] = data.str_size();
		BOOST_STATIC_ASSERT(sizeof(SortableStrVec::SEntry) == 4*3);
		m_rows.offsets.risk_set_data(offsets);
		m_rows.offsets.risk_set_size(rows + 1);
		m_rows.offsets.risk_set_capacity(3 * rows);
		m_rows.offsets.shrink_to_fit();
		data.m_index.risk_release_ownership();
	#if !defined(NDEBUG)
		assert(data.m_strpool.size() == m_rows.offsets.back());
		for (size_t i = 0; i < rows; ++i) {
			assert(m_rows.offsets[i] < m_rows.offsets[i+1]);
		}
	#endif
	}
	m_rows.strpool.swap((valvec<char>&)data.m_strpool);
	m_fixedLen = fixlen;
}

void MockReadonlyStore::save(fstring path1) const {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "wb");
	fp.disbuf();
	NativeDataOutput<OutputBuffer> dio; dio.attach(&fp);
	size_t rows = m_fixedLen ? m_rows.strpool.size() / m_fixedLen : m_rows.size();
	dio << uint64_t(m_fixedLen);
	dio << uint64_t(rows);
	dio << uint64_t(m_rows.strpool.size());
	if (0 == m_fixedLen) {
	#if !defined(NDEBUG)
		assert(m_rows.strpool.size() == m_rows.offsets.back());
		for (size_t i = 0; i < rows; ++i) {
			assert(m_rows.offsets[i] < m_rows.offsets[i+1]);
		}
	#endif
		dio.ensureWrite(m_rows.offsets.data(), m_rows.offsets.used_mem_size());
	} else {
		assert(m_rows.strpool.size() % m_fixedLen == 0);
	}
	dio.ensureWrite(m_rows.strpool.data(), m_rows.strpool.used_mem_size());
}
void MockReadonlyStore::load(fstring path1) {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "rb");
	fp.disbuf();
	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	uint64_t fixlen, rows, strSize;
	dio >> fixlen;
	dio >> rows;
	dio >> strSize;
	m_fixedLen = size_t(fixlen);
	m_rows.strpool.resize_no_init(size_t(strSize));
	if (0 == m_fixedLen) {
		m_rows.offsets.resize_no_init(size_t(rows + 1));
		dio.ensureRead(m_rows.offsets.data(), m_rows.offsets.used_mem_size());
	#if !defined(NDEBUG)
		assert(m_rows.strpool.size() == m_rows.offsets.back());
		for (size_t i = 0; i < rows; ++i) {
			assert(m_rows.offsets[i] < m_rows.offsets[i+1]);
		}
	#endif
	} else {
		assert(m_rows.strpool.size() % m_fixedLen == 0);
		assert(m_rows.strpool.size() / m_fixedLen == rows);
	}
	dio.ensureRead(m_rows.strpool.data(), m_rows.strpool.used_mem_size());
}

struct FixedLenKeyCompare {
	bool operator()(size_t x, fstring y) const {
		fstring xs(strpool + fixedLen * x, fixedLen);
		return schema->compareData(xs, y) < 0;
	}
	bool operator()(fstring x, size_t y) const {
		return (*this)(y, x);
	}
	size_t fixedLen;
	const char  * strpool;
	const Schema* schema;
};

struct VarLenKeyCompare {
	bool operator()(size_t x, fstring y) const {
		size_t xoff0 = offsets[x], xoff1 = offsets[x+1];
		fstring xs(strpool + xoff0, xoff1 - xoff0);
		return schema->compareData(xs, y) < 0;
	}
	bool operator()(fstring x, size_t y) const {
		return (*this)(y, x);
	}
	const char    * strpool;
	const uint32_t* offsets;
	const Schema  * schema;
};

class MockReadonlyIndexIterator : public IndexIterator {
	friend class MockReadonlyIndex;
	MockReadonlyIndexPtr m_index;
	size_t m_pos = size_t(-1);
public:
	MockReadonlyIndexIterator(const MockReadonlyIndex* owner) {
		m_index.reset(const_cast<MockReadonlyIndex*>(owner));
	}
	bool increment(llong* id, valvec<byte>* key) override {
		auto owner = static_cast<const MockReadonlyIndex*>(m_index.get());
		assert(nullptr != id);
		if (nark_unlikely(size_t(-1) == m_pos)) {
			m_pos = 1;
			getIndexKey(id, key, owner, 0);
			return true;
		}
		if (nark_likely(m_pos < owner->m_ids.size())) {
			getIndexKey(id, key, owner, m_pos++);
			return true;
		}
		return false;
	}
	bool decrement(llong* id, valvec<byte>* key) override {
		auto owner = static_cast<const MockReadonlyIndex*>(m_index.get());
		if (nark_unlikely(size_t(-1) == m_pos)) {
			m_pos = owner->m_ids.size() - 1;
			getIndexKey(id, key, owner, m_pos);
			return true;
		}
		if (nark_likely(m_pos > 0)) {
			getIndexKey(id, key, owner, --m_pos);
			return true;
		}
		return false;
	}
	void reset(PermanentablePtr p2) {
		assert(!p2 || dynamic_cast<MockReadonlyIndex*>(p2.get()));
		m_index.reset(dynamic_cast<MockReadonlyIndex*>(p2.get()));
		m_pos = size_t(-1);
	}
	bool seekExact(fstring key) override {
		size_t lo;
		if (seekLowerBound_imp(key, &lo)) {
			m_pos = lo;
			return true;
		}
		return false;
	}
	bool seekLowerBound(fstring key) override {
		return seekLowerBound_imp(key, &m_pos);
	}
	bool seekLowerBound_imp(fstring key, size_t* pLower) {
		auto owner = static_cast<const MockReadonlyIndex*>(m_index.get());
		const uint32_t* index = owner->m_ids.data();
		const size_t rows = owner->m_ids.size();
		const size_t fixlen = owner->m_fixedLen;
		if (fixlen) {
			assert(owner->m_keys.size() == 0);
			FixedLenKeyCompare cmp;
			cmp.fixedLen = fixlen;
			cmp.strpool = owner->m_keys.strpool.data();
			cmp.schema = owner->m_schema.get();
			size_t lo = nark::lower_bound_0(index, rows, key, cmp);
			*pLower = lo;
			if (lo < rows && key == owner->m_keys[lo]) {
				return true;
			}
		}
		else {
			VarLenKeyCompare cmp;
			cmp.offsets = owner->m_keys.offsets.data();
			cmp.strpool = owner->m_keys.strpool.data();
			cmp.schema = owner->m_schema.get();
			size_t lo = nark::lower_bound_0(index, rows, key, cmp);
			*pLower = lo;
			if (lo < rows && key == owner->m_keys[lo]) {
				return true;
			}
		}
		return false;
	}

	void getIndexKey(llong* id, valvec<byte>* key,
					 const MockReadonlyIndex* owner, size_t pos) {
		assert(pos < owner->m_ids.size());
		*id = owner->m_ids[pos];
		if (key) {
			fstring k = owner->m_keys[*id];
			key->assign(k.udata(), k.size());
		}
	}
};

MockReadonlyIndex::MockReadonlyIndex(SchemaPtr schema) {
	m_schema = schema;
}

MockReadonlyIndex::~MockReadonlyIndex() {
}

StoreIteratorPtr MockReadonlyIndex::createStoreIter(DbContext*) const {
	assert(!"Readonly column store did not define iterator");
	return nullptr;
}

#ifdef _MSC_VER
#define qsort_r qsort_s
#endif

void
MockReadonlyIndex::build(SortableStrVec& keys) {
	const Schema* schema = m_schema.get();
	const byte* base = keys.m_strpool.data();
	size_t fixlen = schema->getFixedRowLen();
	if (fixlen) {
		assert(keys.m_index.size() == 0);
		assert(keys.str_size() % fixlen == 0);
		m_ids.resize_no_init(keys.str_size() / fixlen);
		for (size_t i = 0; i < m_ids.size(); ++i) m_ids[i] = i;
		std::sort(m_ids.begin(), m_ids.end(), [=](size_t x, size_t y) {
			fstring xs(base + fixlen * x, fixlen);
			fstring ys(base + fixlen * y, fixlen);
			int r = schema->compareData(xs, ys);
			if (r) return r < 0;
			else   return x < y;
		});
	}
	else {
		if (keys.str_size() >= UINT32_MAX) {
			THROW_STD(length_error,
				"keys.str_size=%lld is too large", llong(keys.str_size()));
		}
		// reuse memory of keys.m_index
		auto offsets = (uint32_t*)keys.m_index.data();
		size_t rows = keys.m_index.size();
		m_ids.resize_no_init(rows);
		for (size_t i = 0; i < rows; ++i) m_ids[i] = i;
		for (size_t i = 0; i < rows; ++i) {
			uint32_t offset = uint32_t(keys.m_index[i].offset);
			offsets[i] = offset;
		}
		offsets[rows] = keys.str_size();
		std::sort(m_ids.begin(), m_ids.end(), [=](size_t x, size_t y) {
			size_t xoff0 = offsets[x], xoff1 = offsets[x+1];
			size_t yoff0 = offsets[y], yoff1 = offsets[y+1];
			fstring xs(base + xoff0, xoff1 - xoff0);
			fstring ys(base + yoff0, yoff1 - yoff0);
			int r = schema->compareData(xs, ys);
			if (r) return r < 0;
			else   return x < y;
		});
		BOOST_STATIC_ASSERT(sizeof(SortableStrVec::SEntry) == 4*3);
		m_keys.offsets.risk_set_data(offsets);
		m_keys.offsets.risk_set_size(rows + 1);
		m_keys.offsets.risk_set_capacity(3 * rows);
		m_keys.offsets.shrink_to_fit();
		keys.m_index.risk_release_ownership();
	}
	m_keys.strpool.swap((valvec<char>&)keys.m_strpool);
	m_fixedLen = fixlen;
}

void MockReadonlyIndex::save(fstring path1) const {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "wb");
	fp.disbuf();
	NativeDataOutput<OutputBuffer> dio; dio.attach(&fp);
	size_t rows = m_ids.size();
	dio << uint64_t(m_fixedLen);
	dio << uint64_t(rows);
	dio << uint64_t(m_keys.strpool.size());
	dio.ensureWrite(m_ids.data(), m_ids.used_mem_size());
	if (m_fixedLen) {
		assert(m_keys.size() == 0);
		assert(m_keys.strpool.size() == m_fixedLen * rows);
	} else {
		assert(m_keys.size() == rows);
		dio.ensureWrite(m_keys.offsets.data(), m_keys.offsets.used_mem_size());
	}
	dio.ensureWrite(m_keys.strpool.data(), m_keys.strpool.used_mem_size());
}

void MockReadonlyIndex::load(fstring path1) {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "rb");
	fp.disbuf();
	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	uint64_t fixlen, rows, keylen;
	dio >> fixlen;
	dio >> rows;
	dio >> keylen;
	m_ids.resize_no_init(size_t(rows));
	dio.ensureRead(m_ids.data(), m_ids.used_mem_size());
	if (0 == fixlen) {
		m_keys.offsets.resize_no_init(size_t(rows + 1));
		dio.ensureRead(m_keys.offsets.data(), m_keys.offsets.used_mem_size());
	}
	else {
		assert(fixlen * rows == keylen);
	}
	m_keys.strpool.resize_no_init(size_t(keylen));
	dio.ensureRead(m_keys.strpool.data(), size_t(keylen));
	m_fixedLen = size_t(fixlen);
}

llong MockReadonlyIndex::numDataRows() const {
	return m_ids.size();
}
llong MockReadonlyIndex::dataStorageSize() const {
	return m_ids.used_mem_size()
		+ m_keys.offsets.used_mem_size()
		+ m_keys.strpool.used_mem_size();
}

void MockReadonlyIndex::getValueAppend(llong id, valvec<byte>* key, DbContext*) const {
	assert(id < (llong)m_ids.size());
	assert(id >= 0);
	if (m_fixedLen) {
		assert(m_keys.size() == 0);
		assert(0 == llong(m_keys.strpool.size() % m_fixedLen));
		assert(m_keys.strpool.size() == m_ids.size() * m_fixedLen);
		fstring key1(m_keys.strpool.data() + m_fixedLen * id, m_fixedLen);
		key->append(key1.udata(), key1.size());
	}
	else {
		assert(m_ids.size() == m_keys.size());
		fstring key1 = m_keys[id];
		key->append(key1.udata(), key1.size());
	}
}

IndexIteratorPtr MockReadonlyIndex::createIndexIter(DbContext*) const {
	return new MockReadonlyIndexIterator(this);
}

llong MockReadonlyIndex::numIndexRows() const {
	return m_ids.size();
}

llong MockReadonlyIndex::indexStorageSize() const {
	return m_ids.used_mem_size() + m_keys.offsets.used_mem_size();
}

//////////////////////////////////////////////////////////////////
template<class WrStore>
class MockWritableStoreIter : public StoreIterator {
	size_t m_id;
public:
	MockWritableStoreIter(const WrStore* store) {
		m_store.reset(const_cast<WrStore*>(store));
		m_id = 0;
	}
	bool increment(llong* id, valvec<byte>* val) override {
		auto store = static_cast<WrStore*>(m_store.get());
		if (m_id < store->m_rows.size()) {
			*id = m_id;
			*val = store->m_rows[m_id];
			m_id++;
			return true;
		}
		return false;
	}
};

void MockWritableStore::save(fstring path1) const {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "wb");
	fp.disbuf();
	NativeDataOutput<OutputBuffer> dio; dio.attach(&fp);
	dio << m_rows;
}
void MockWritableStore::load(fstring path1) {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "rb");
	fp.disbuf();
	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	dio >> m_rows;
}

llong MockWritableStore::dataStorageSize() const {
	return m_rows.used_mem_size() + m_dataSize;
}

llong MockWritableStore::numDataRows() const {
	return m_rows.size();
}

void MockWritableStore::getValueAppend(llong id, valvec<byte>* val, DbContext*) const {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	val->append(m_rows[id]);
}

StoreIteratorPtr MockWritableStore::createStoreIter(DbContext*) const {
	return new MockWritableStoreIter<MockWritableStore>(this);
}

llong MockWritableStore::append(fstring row, DbContext*) {
	llong id = m_rows.size();
	m_rows.push_back();
	m_rows.back().assign(row);
	return id;
}
void MockWritableStore::replace(llong id, fstring row, DbContext*) {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	m_rows[id].assign(row);
}
void MockWritableStore::remove(llong id, DbContext*) {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	m_rows[id].clear();
}

//////////////////////////////////////////////////////////////////

template<class Key>
class MockWritableIndex<Key>::MyIndexIter : public IndexIterator {
	typedef boost::intrusive_ptr<MockWritableIndex> MockWritableIndexPtr;
	MockWritableIndexPtr m_index;
	typename std::set<kv_t>::const_iterator m_iter;
	bool m_isNull;
public:
	MyIndexIter(const MockWritableIndex* owner) {
		m_index.reset(const_cast<MockWritableIndex*>(owner));
		m_isNull = true;
	}
	bool increment(llong* id, valvec<byte>* key) override {
		auto owner = static_cast<const MockWritableIndex*>(m_index.get());
		if (nark_unlikely(m_isNull)) {
			m_isNull = false;
			m_iter = owner->m_kv.begin();
			if (!owner->m_kv.empty()) {
				*id = m_iter->second;
				copyKey(m_iter->first, key);
				++m_iter;
				return true;
			}
			return false;
		}
		if (nark_likely(owner->m_kv.end() != m_iter)) {
			*id = m_iter->second;
			copyKey(m_iter->first, key);
			++m_iter;
			return true;
		}
		return false;
	}
	bool decrement(llong* id, valvec<byte>* key) override {
		auto owner = static_cast<const MockWritableIndex*>(m_index.get());
		if (nark_unlikely(m_isNull)) {
			m_isNull = false;
			m_iter = owner->m_kv.end();
			if (!owner->m_kv.empty()) {
				--m_iter;
				*id = m_iter->second;
				copyKey(m_iter->first, key);
				return true;
			}
			return false;
		}
		if (nark_likely(owner->m_kv.begin() != m_iter)) {
			--m_iter;
			*id = m_iter->second;
			copyKey(m_iter->first, key);
			return true;
		}
		return false;
	}
	void reset(PermanentablePtr p2) {
		assert(!p2 || dynamic_cast<MockWritableIndex*>(p2.get()));
		if (p2)
			m_index.reset(dynamic_cast<MockWritableIndex*>(p2.get()));
		m_isNull = true;
	}
	bool seekExact(fstring key) override {
		auto owner = static_cast<const MockWritableIndex*>(m_index.get());
		auto kv = std::make_pair(makeKey(key), 0LL);
		auto iter = owner->m_kv.lower_bound(kv);
		if (owner->m_kv.end() != iter && iter->first == kv.first) {
			m_iter = iter;
			m_isNull = false;
			return true;
		}
		return false;
	}
	bool seekLowerBound(fstring key) override {
		auto owner = static_cast<const MockWritableIndex*>(m_index.get());
		auto kv = std::make_pair(makeKey(key), 0LL);
		auto iter = owner->m_kv.lower_bound(kv);
		m_iter = iter;
		m_isNull = false;
		if (owner->m_kv.end() != iter && iter->first == kv.first) {
			return true;
		}
		return false;
	}
private:
	static void copyKey(const std::string& src, valvec<byte>* dst) {
		dst->assign((const char*)src.data(), src.size());
	}
	template<class PrimitiveKey>
	static void copyKey(const PrimitiveKey& src, valvec<byte>* dst) {
		BOOST_STATIC_ASSERT(boost::is_pod<PrimitiveKey>::value);
		dst->assign((const char*)&src, sizeof(PrimitiveKey));
	}

	template<class Primitive>
	static Primitive makeKeyImp(fstring key, Primitive*) {
		assert(key.size() == sizeof(Primitive));
		return unaligned_load<Primitive>(key.udata());
	}
	static std::string makeKeyImp(fstring key, std::string*) { return key.str(); }
public:
	static Key makeKey(fstring key) { return makeKeyImp(key, (Key*)0); }
	template<class Primitive>
	static size_t keyHeapLen(const Primitive&) { return 0; }
	static size_t keyHeapLen(const std::string& x) { return x.size() + 1; }
};

template<class Key>
IndexIteratorPtr MockWritableIndex<Key>::createIndexIter(DbContext*) const {
	return new MyIndexIter(this);
}

template<class Key>
void MockWritableIndex<Key>::save(fstring path1) const {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "wb");
	fp.disbuf();
	NativeDataOutput<OutputBuffer> dio; dio.attach(&fp);
	dio << m_kv;
}
template<class Key>
void MockWritableIndex<Key>::load(fstring path1) {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "rb");
	fp.disbuf();
	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	dio >> m_kv;
}

template<class Key>
llong MockWritableIndex<Key>::numIndexRows() const {
	return m_kv.size();
}

template<class Key>
llong MockWritableIndex<Key>::indexStorageSize() const {
	// std::set's rbtree node needs 4ptr space
	size_t size = m_kv.size() * (sizeof(kv_t) + 4 * sizeof(void*));
	return m_keysLen + size;
}

template<class Key>
size_t MockWritableIndex<Key>::insert(fstring key, llong id, DbContext*) {
	auto ib = m_kv.insert(std::make_pair(MyIndexIter::makeKey(key), id));
	if (ib.second) {
		m_keysLen += MyIndexIter::keyHeapLen(ib.first->first);
	}
	return ib.second;
}

template<class Key>
size_t MockWritableIndex<Key>::replace(fstring key, llong oldId, llong newId, DbContext*) {
	auto kx = MyIndexIter::makeKey(key);
	if (oldId != newId) {
		m_kv.erase(std::make_pair(kx, oldId));
	}
	auto ib = m_kv.insert(std::make_pair(kx, newId));
	return ib.second;
}

template<class Key>
size_t MockWritableIndex<Key>::remove(fstring key, llong id, DbContext*) {
	auto iter = m_kv.find(std::make_pair(MyIndexIter::makeKey(key), id));
	if (m_kv.end() != iter) {
		m_keysLen = MyIndexIter::keyHeapLen(iter->first);
		m_kv.erase(iter);
		return 1;
	}
	return 0;
}

template<class Key>
void MockWritableIndex<Key>::flush() {
	// do nothing
}

///////////////////////////////////////////////////////////////////////
MockReadonlySegment::MockReadonlySegment() {
}
MockReadonlySegment::~MockReadonlySegment() {
}

ReadableStorePtr
MockReadonlySegment::openPart(fstring path) const {
	// Mock just use one kind of data store
//	FileStream fp(path.c_str(), "rb");
//	fp.disbuf();
//	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	ReadableStorePtr store(new MockReadonlyStore());
	store->load(path);
	return store;
}

ReadableIndexStorePtr
MockReadonlySegment::openIndex(fstring path, SchemaPtr schema) const {
	ReadableIndexStorePtr store(new MockReadonlyIndex(schema));
	store->load(path);
	return store;
}

ReadableIndexStorePtr
MockReadonlySegment::buildIndex(SchemaPtr indexSchema,
								SortableStrVec& indexData)
const {
	std::unique_ptr<MockReadonlyIndex> index(new MockReadonlyIndex(indexSchema));
	index->build(indexData);
	return index.release();
}

ReadableStorePtr
MockReadonlySegment::buildStore(SortableStrVec& storeData) const {
	std::unique_ptr<MockReadonlyStore> store(new MockReadonlyStore());
	store->build(this->m_rowSchema, storeData);
	return store.release();
}

///////////////////////////////////////////////////////////////////////////
MockWritableSegment::MockWritableSegment(fstring dir) {
	m_segDir = dir.str();
}
MockWritableSegment::~MockWritableSegment() {
	if (!m_tobeDel)
		this->save(m_segDir);
}

void MockWritableSegment::save(fstring dir) const {
	PlainWritableSegment::save(dir);
	saveIndices(dir);
	fs::path fpath = dir.c_str();
	fpath /= "rows";
	FileStream fp(fpath.string().c_str(), "wb");
	fp.disbuf();
	NativeDataOutput<OutputBuffer> dio; dio.attach(&fp);
	dio << m_rows;
}

void MockWritableSegment::load(fstring dir) {
	PlainWritableSegment::load(dir);
	this->openIndices(dir);
	fs::path fpath = dir.c_str();
	fpath /= "/rows";
	FileStream fp(fpath.string().c_str(), "rb");
	fp.disbuf();
	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	dio >> m_rows;
}

WritableIndexPtr
MockWritableSegment::openIndex(fstring path, SchemaPtr schema) const {
	WritableIndexPtr index = createIndex(path, schema);
	index->load(path);
	return index;
}

llong MockWritableSegment::dataStorageSize() const {
	return m_rows.used_mem_size() + m_dataSize;
}

void
MockWritableSegment::getValueAppend(llong id, valvec<byte>* val,
									DbContext*)
const {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	val->append(m_rows[id]);
}

StoreIteratorPtr MockWritableSegment::createStoreIter(DbContext*) const {
	return StoreIteratorPtr(new MockWritableStoreIter<MockWritableSegment>(this));
}

llong MockWritableSegment::totalStorageSize() const {
	return totalIndexSize() + m_rows.used_mem_size() + m_dataSize;
}

llong MockWritableSegment::append(fstring row, DbContext*) {
	llong id = m_rows.size();
	m_rows.push_back();
	m_rows.back().assign(row);
	m_dataSize += row.size();
	return id;
}

void MockWritableSegment::replace(llong id, fstring row, DbContext*) {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	size_t oldsize = m_rows[id].size();
	m_rows[id].assign(row);
	m_dataSize -= oldsize;
	m_dataSize += row.size();
}

void MockWritableSegment::remove(llong id, DbContext*) {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	m_dataSize -= m_rows[id].size();
	m_rows[id].clear();
}

void MockWritableSegment::flush() {
	// do nothing
}

WritableIndexPtr MockWritableSegment::createIndex(fstring, SchemaPtr schema) const {
	WritableIndexPtr index;
	if (schema->columnNum() == 1) {
		ColumnMeta cm = schema->getColumnMeta(0);
#define CASE_COL_TYPE(Enum, Type) \
		case ColumnType::Enum: return new MockWritableIndex<Type>();
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		switch (cm.type) {
			CASE_COL_TYPE(Uint08, uint8_t);
			CASE_COL_TYPE(Sint08,  int8_t);
			CASE_COL_TYPE(Uint16, uint16_t);
			CASE_COL_TYPE(Sint16,  int16_t);
			CASE_COL_TYPE(Uint32, uint32_t);
			CASE_COL_TYPE(Sint32,  int32_t);
			CASE_COL_TYPE(Uint64, uint64_t);
			CASE_COL_TYPE(Sint64,  int64_t);
			CASE_COL_TYPE(Float32, float);
			CASE_COL_TYPE(Float64, double);
		}
#undef CASE_COL_TYPE
	}
	return new MockWritableIndex<std::string>();
}

///////////////////////////////////////////////////////////////////////////

MockDbContext::MockDbContext(const CompositeTable* tab) : DbContext(tab) {
}
MockDbContext::~MockDbContext() {
}

DbContextPtr MockCompositeTable::createDbContext() const {
	return new MockDbContext(this);
}

ReadonlySegmentPtr
MockCompositeTable::createReadonlySegment(fstring dir) const {
	std::unique_ptr<MockReadonlySegment> seg(new MockReadonlySegment());
	return seg.release();
}

WritableSegmentPtr
MockCompositeTable::createWritableSegment(fstring dir) const {
	std::unique_ptr<MockWritableSegment> seg(new MockWritableSegment(dir));
	return seg.release();
}

WritableSegmentPtr
MockCompositeTable::openWritableSegment(fstring dir) const {
	WritableSegmentPtr seg(new MockWritableSegment(dir));
	seg->m_rowSchema = m_rowSchema;
	seg->m_indexSchemaSet = m_indexSchemaSet;
	seg->m_nonIndexRowSchema = m_nonIndexRowSchema;
	seg->load(dir);
	return seg;
}

} } // namespace nark::db