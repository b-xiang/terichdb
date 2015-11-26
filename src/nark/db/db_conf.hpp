#ifndef __nark_db_db_conf_hpp__
#define __nark_db_db_conf_hpp__

#include <string>
#include <nark/hash_strmap.hpp>
#include <nark/gold_hash_map.hpp>
#include <nark/bitmap.hpp>
#include <nark/pass_by_value.hpp>
#include <nark/util/fstrvec.hpp>
#include <nark/util/refcount.hpp>
#include <boost/intrusive_ptr.hpp>

#if defined(_MSC_VER)

#  if defined(NARK_DB_CREATE_DLL)
#    pragma warning(disable: 4251)
#    define NARK_DB_DLL __declspec(dllexport)      // creator of dll
#    if defined(_DEBUG) || !defined(NDEBUG)
#//	   pragma message("creating nark-db-d.lib")
#    else
#//	   pragma message("creating nark-db-r.lib")
#    endif
#  elif defined(NARK_DB_USE_DLL)
#    pragma warning(disable: 4251)
#    define NARK_DB_DLL __declspec(dllimport)      // user of dll
#    if defined(_DEBUG) || !defined(NDEBUG)
//#	   pragma comment(lib, "nark-db-d.lib")
#    else
//#	   pragma comment(lib, "nark-db-r.lib")
#    endif
#  else
#    define NARK_DB_DLL                            // static lib creator or user
#  endif

#else /* _MSC_VER */

#  define NARK_DB_DLL

#endif /* _MSC_VER */


namespace nark { namespace db {

	struct ClassMember_name {
		template<class X, class Y>
		bool operator()(const X& x, const Y& y) const { return x < y; }
		template<class T>
		const std::string& operator()(const T& x) const { return x.name; }
	};

	enum class SortOrder : unsigned char {
		Ascending,
		Descending,
		UnOrdered,
	};

	enum class ColumnType : unsigned char {
		// all number types are LittleEndian
		Uint08,
		Sint08,
		Uint16,
		Sint16,
		Uint32,
		Sint32,
		Uint64,
		Sint64,
		Uint128,
		Sint128,
		Float32,
		Float64,
		Float128,
		Uuid,    // 16 bytes(128 bits) binary
		Fixed,   // Fixed length binary
		StrZero, // Zero ended string
		StrUtf8, // Prefixed by length(var_uint) in bytes
		Binary,  // Prefixed by length(var_uint) in bytes
	};

	struct NARK_DB_DLL ColumnMeta {
		uint32_t fixedLen = 0;
		static_bitmap<16, uint16_t> flags;
		ColumnType type;
		SortOrder  order;
		ColumnMeta();
		explicit ColumnMeta(ColumnType, SortOrder ord = SortOrder::UnOrdered);
	};

	class NARK_DB_DLL Schema : public RefCounter {
		friend class SchemaSet;
	public:
		Schema();
		~Schema();
		void compile(const Schema* parent = nullptr);

		void parseRow(fstring row, valvec<fstring>* columns) const;
		void parseRowAppend(fstring row, valvec<fstring>* columns) const;

		void combineRow(const valvec<fstring>& myCols, valvec<byte>* myRowData) const;

		void selectParent(const valvec<fstring>& parentCols, valvec<byte>* myRowData) const;
		void selectParent(const valvec<fstring>& parentCols, valvec<fstring>* myCols) const;

		size_t parentColumnId(size_t myColumnId) const {
			assert(m_proj.size() == m_columnsMeta.end_i());
			assert(myColumnId < m_proj.size());
			return m_proj[myColumnId];
		}

		std::string toJsonStr(fstring row) const;

		ColumnType getColumnType(size_t columnId) const;
		fstring getColumnName(size_t columnId) const;
		size_t getColumnId(fstring columnName) const;
		const ColumnMeta& getColumnMeta(size_t columnId) const;
		size_t columnNum() const { return m_columnsMeta.end_i(); }

		size_t getFixedRowLen() const { return m_fixedLen; }

		static ColumnType parseColumnType(fstring str);
		static const char* columnTypeStr(ColumnType);

		std::string joinColumnNames(char delim = ',') const;

		int compareData(fstring x, fstring y) const;

		// used by glibc.qsort_r or msvc.qsort_s
		struct CompareByIndexContext {
			const Schema* schema;
			const char*   basePtr;
			const uint32_t* offsets;
		};
		static int QsortCompareFixedLen(const void* x, const void* y, const void* ctx);
		static int QsortCompareByIndex(const void* x, const void* y, const void* ctx);

		hash_strmap<ColumnMeta> m_columnsMeta;

	protected:
		void compileProject(const Schema* parent);
		size_t computeFixedRowLen() const; // return 0 if RowLen is not fixed

	protected:
		size_t m_fixedLen;
	/*
	// Backlog: select from multiple tables
		struct ColumnLink {
			const Schema* parent;
			size_t        proj; // column[i] is from parent->column[proj[i]]
		};
		valvec<ColumnLink> m_columnsLink;
	*/
		const Schema*    m_parent;
		valvec<unsigned> m_proj;

	public:
		// Helpers for define & serializing object
		template<int N>
		struct Fixed {
			char data[N];
			template<class DIO> friend void
			DataIO_loadObject(DIO& dio, Fixed& x) { dio.ensureRead(&x, N); }
			template<class DIO> friend void
			DataIO_saveObject(DIO& dio,const Fixed&x){dio.ensureWrite(&x,N);}
			template<class DIO>
			friend boost::mpl::true_
			Deduce_DataIO_is_dump(DIO*,Fixed*){return boost::mpl::true_();}
			friend boost::mpl::false_
			Deduce_DataIO_need_bswap(Fixed*){return boost::mpl::false_();}
		};
		template<class Str>
		class StrZeroLoader {
			Str* str;
		public:
			StrZeroLoader(Str& s) : str(&s) {}
			template<class DataIO>
			friend void DataIO_loadObject(DataIO& dio, StrZeroLoader& x) {
				unsigned char c;
				do { dio >> c;
					 x.str->push_back(c);
				} while (0 != c);
			}
		};
		template<class Str>
		class StrZeroSaver : public fstring {
		public:
			StrZeroSaver(const Str& x) : fstring(x.data(), x.size()) {}
			template<class DataIO>
			friend void DataIO_saveObject(DataIO&dio,const StrZeroSaver&x){
				if (x.size() == 0)
					dio << (unsigned char)(0);
				else {
					dio.ensureWrite(x.data(), x.size());
					if (0 != x[x.n-1])
						dio << (unsigned char)(0);
				}
			}
		};
		// StrZero will not be serialized as Last Column
		template<class Str>
		static pass_by_value<StrZeroLoader<Str> >
		StrZero(Str& x) { return StrZeroLoader<Str>(x); }
		template<class Str>
		static StrZeroSaver<Str>
		StrZero(const Str& x) { return StrZeroSaver<Str>(x); }
	};
	typedef boost::intrusive_ptr<Schema> SchemaPtr;

	// a set of schema, could be all indices of a table
	// or all column groups of a table
	class NARK_DB_DLL SchemaSet : public RefCounter {
		struct Hash {
			size_t operator()(const SchemaPtr& x) const;
			size_t operator()(fstring x) const;
		};
		struct Equal {
			bool operator()(const SchemaPtr& x, const SchemaPtr& y) const;
			bool operator()(const SchemaPtr& x, fstring y) const;
			bool operator()(fstring x, const SchemaPtr& y) const
			  { return (*this)(y, x); }
		};
	public:
		gold_hash_set<SchemaPtr, Hash, Equal> m_nested;
		febitvec m_keepColumn;
		febitvec m_keepSchema;
		void compileSchemaSet(const Schema* parent);
	};
	typedef boost::intrusive_ptr<SchemaSet> SchemaSetPtr;

	struct NARK_DB_DLL DbConf {
		std::string dir;
	};

} } // namespace nark::db

#endif //__nark_db_db_conf_hpp__