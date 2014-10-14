#ifndef BIZ_MSG_H
#define BIZ_MSG_H

#include "ssdb.h"

/* 
业务逻辑
支持事务
*/

static inline std::string encode_msg_meta_key(const Bytes &key) {
	std::string buf;
	buf.append(1, DataType::MM);
	buf.append(key.data(), key.size());
	return buf;
}

static inline
std::string encode_msg_key(const Bytes &key, uint32_t seq){
	std::string buf;
	buf.append(1, DataType::MK);
	buf.append(key.data(), key.size());
	buf.append((char*)&seq, 4);
	return buf;
}

#endif //BIZ_MSG_H
