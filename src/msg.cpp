#include <string>
#include <list>
#include <cstdint>
#include "msg.h"
#include "lockpool.h"

#define MAXROWSPERKEY 3
#define MAXROWS MAXROWSPERKEY*2

static LockPool pool;

class Meta {
private:
	uint32_t _idx;
	uint16_t _rows;
public:
	Meta(const int k, int r):_idx(k), _rows(r) {}
	Meta():_rows(0) {}
	const uint16_t Rows() {
		return _rows;
	}

	void SetRows(const uint16_t r) {
		_rows = r;
	}

	void SetIndex(const int k) {
		_idx = k;
	}

	void Reset() {
		_idx = 0;
		_rows = 0;
	}

	const int Index() {
		return _idx;
	}

};

static inline void getMetaInfo(const char *p, uint32_t &s, uint32_t &seq) {
	s = *((uint32_t*)p);
	p += 4;
	seq = *((uint32_t*)p);
	p += 4;
}

static int parseMeta(const std::string& r, std::list<Meta> &v, uint32_t &seq) {
	uint32_t s = 0;
	if(r.length() < 8)
		return -1;
	char* ptr = (char*)r.data();
	char *end = ptr + r.length();
	getMetaInfo(ptr, s, seq);
	
	ptr += 8;
	for(int i = 0; i < s && ptr != end; i++) {
		uint16_t idx = *(uint32_t*)ptr;
		if((end - ptr) < 4)
			return -1;
		ptr += 4;
		uint16_t rows = *(uint16_t*)ptr;
		if((end - ptr) < 2)
			return -1;
		ptr += 2;
		v.push_back(Meta(idx, rows));
	}
	return s;
}

static std::string marshal_metas(std::list<Meta> &metas, uint32_t total, uint32_t seq) {
	std::string b;
	b.append((char*)&total, 4);
	b.append((char*)&seq, 4);
	std::for_each(metas.begin(), metas.end(), [&b](Meta &m){
		uint32_t idx = m.Index();
		b.append((char*)&idx, 4);
		uint16_t r = m.Rows();
		b.append((char*)&r, 2);
	});

	return std::move(b);
}

static void reachMaxLimits(const Bytes &key, uint32_t &total, std::list<Meta> &metas, BinlogQueue *binlogs, const char log_type) {
	auto fmeta = metas.front();
	metas.pop_front();
	total -= fmeta.Rows();

	//delete the data
	std::string mk = encode_msg_key(key, fmeta.Index());

	binlogs->Delete(mk);
	binlogs->add_log(log_type, BinlogCommand::KDEL, mk);
	//avoid seq repeat cover. 避免seq 达到最大值后覆盖, 基本想法当seq达到上限后 key 重新命名。
}

int SSDB::msg_rows(const Bytes &key) {
	if(key.empty()){
		log_error("empty key!");
		return -1;
	}
	
	std::string metakey = encode_msg_meta_key(key);
	Lock lock = pool.getLock(metakey);
	std::string metaRaw;
	int found = this->msg_raw_get(Bytes(metakey), &metaRaw);
	if(!found)
		return 0;
	uint32_t size;
	uint32_t seq;
	getMetaInfo(metaRaw.data(), size, seq);
	return size;
}

int SSDB::msg_append(const Bytes &key, const Bytes &val, char log_type) {
	if(key.empty()){
		log_error("empty key!");
		return -1;
	}
	std::list<Meta> metas;
	std::string metaRaw, raw;
	std::string metakey = encode_msg_meta_key(key);
	Lock lock = pool.getLock(metakey);

	int found = this->msg_raw_get(Bytes(metakey), &metaRaw);
	uint32_t total = 0;
	uint32_t seq = 0;
	if(found) {
		
		int rs = parseMeta(metaRaw, metas, seq);
		if(rs < 0) {
			log_error("parse meta error!");
			return 0;
		}
		total = (uint32_t)rs;
	}
	Meta meta;
	std::string mk;
	if(metas.size() > 0) {
		meta = metas.back();
		mk = encode_msg_key(key, meta.Index());
		//每个数据key 的记录 不能超过限制.
		if(meta.Rows() >= MAXROWSPERKEY) {
			//设置为空 便于重新生成key
			mk = "";
		} else {
			metas.pop_back();
		}
	}
	
	if(mk.length() == 0) {
		seq++;
		meta.Reset();
		meta.SetIndex(seq);
		mk = encode_msg_key(key, meta.Index());
	}
	meta.SetRows(meta.Rows()+1);
	metas.push_back(meta);

	total += 1;

	//如果超过最大条数限制 抛弃最开始的key rows （总会是限制keys + 1） 
	//如 限制总条数是 100  每个key 保存10条记录，那么最多的时候将会有100+9条记录被保存
	Transaction trans(binlogs);
	if(total - MAXROWS == MAXROWSPERKEY) {
		reachMaxLimits(key, total, metas, binlogs, log_type);
	}

	this->msg_raw_get(Bytes(mk), &raw);
	uint16_t len = val.size();
	raw.append((char*)(&len), 2);
	raw.append(val.data(), val.size());

	std::string buf = marshal_metas(metas, total, seq);
	binlogs->Put(metakey, leveldb::Slice(buf));
	binlogs->add_log(log_type, BinlogCommand::KSET, metakey);

	binlogs->Put(mk, raw);
	binlogs->add_log(log_type, BinlogCommand::KSET, mk);
	leveldb::Status s = binlogs->commit();

	if(!s.ok()){
		log_error("msg_append error: %s", s.ToString().c_str());
		return -1;
	}
	
	return total;
}

//需要缓存
int SSDB::msg_raw_get(const Bytes &key, std::string *val) const{
	leveldb::Status s = db->Get(leveldb::ReadOptions(), key.Slice(), val);
	if(s.IsNotFound()){
		return 0;
	}
	if(!s.ok()){
		log_error("get error: %s", s.ToString().c_str());
		return -1;
	}
	return 1;
}

int SSDB::msg_pop_front(const Bytes &key, char log_type) {
	if(key.empty()){
		log_error("empty key!");
		return -1;
	}
	auto metakey = encode_msg_meta_key(key);
	Lock lock = pool.getLock(metakey);
	std::list<Meta> metas;
	std::string metaRaw;
	int found = this->msg_raw_get(Bytes(metakey), &metaRaw);
	uint32_t total = 0;
	uint32_t seq;
	if(found) {
		int rs = parseMeta(metaRaw, metas, seq);
		if(rs < 0) {
			log_error("parse meta error!");
			return 0;
		}
		total = (uint32_t)rs;
	} else {
		return 0;
	}
	
	if(metas.size() == 0) {
		return 0;
	}
	Transaction trans(binlogs);
	auto meta = metas.front();
	metas.pop_front();
	binlogs->begin();
	total -= meta.Rows();
	if(metas.size() == 0) {
		binlogs->Delete(metakey);
		binlogs->add_log(log_type, BinlogCommand::KDEL, metakey);
	} else {
		std::string buf = marshal_metas(metas, total, seq);
		binlogs->Put(metakey, leveldb::Slice(buf));
		binlogs->add_log(log_type, BinlogCommand::KSET, metakey);
	}
	std::string mk = encode_msg_key(key, meta.Index());
	binlogs->Delete(mk);
	binlogs->add_log(log_type, BinlogCommand::KDEL, mk);
	leveldb::Status s = binlogs->commit();
	if(!s.ok()){
		log_error("del error: %s", s.ToString().c_str());
		return -1;
	}
	return total;
}

int SSDB::msg_front(const Bytes &key, std::string *result) {
	if(key.empty()){
		log_error("empty key!");
		return -1;
	}
	auto metakey = encode_msg_meta_key(key);
	Lock lock = pool.getLock(metakey);
	std::list<Meta> metas;
	std::string metaRaw;
	int found = this->msg_raw_get(Bytes(metakey), &metaRaw);
	uint32_t total = 0;
	uint32_t seq;
	if(found) {
		int rs = parseMeta(metaRaw, metas, seq);
		if(rs < 0) {
			log_error("parse meta error!");
			return 0;
		}
		total = (uint32_t)rs;
	} else {
		return 0;
	}
	if(metas.size() == 0)
		return 0;
	auto meta = metas.front();
	return msg_raw_get(Bytes(encode_msg_key(key, meta.Index())), result);
}