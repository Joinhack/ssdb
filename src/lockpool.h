#include <map>
#include <vector>
#include <string>
#include <pthread.h>

class LockPool;

class Lock {
public:
	struct LockHandle {
		pthread_mutex_t mtx;
		std::string key;
		int times;
	};

	Lock(LockHandle *handle, LockPool *pool);

	Lock(Lock &&l) {
		_pool = l._pool;
		_handle = l._handle;
		l._handle = NULL;
		l._pool = NULL;
		lock();
	}

	void lock() {
		pthread_mutex_lock(&_handle->mtx);
	}

	void unlock() {
		pthread_mutex_unlock(&_handle->mtx);
	}
	~Lock();
private:
	LockHandle *_handle;
	LockPool *_pool;
};

class LockPool {
private:
	class L {
	public:
		L(pthread_mutex_t *mtx):_mtx(mtx) {
			Lock();
		}
		void Lock() {
			pthread_mutex_lock(_mtx);
		}

		void Unlock() {
			pthread_mutex_unlock(_mtx);
		}
		~L() {
			Unlock();
		}
		pthread_mutex_t *_mtx;
	};
public:
	LockPool():_max(1000) {
		pthread_mutex_init(&_mtx, NULL);
	}

	Lock getLock(const std::string &k);

	void recycle(Lock::LockHandle *handle);
private:
	int _max;
	std::map<std::string, Lock::LockHandle*> _pool;
	std::vector<Lock::LockHandle*> _cache;
	pthread_mutex_t _mtx;
};

Lock LockPool::getLock(const std::string &k) {
	L l(&_mtx);
	Lock::LockHandle *handle = _pool[k];
	if(handle == NULL) {
		if(_cache.size() > 0) {
			handle = _cache.back();
			_cache.pop_back();
		} else {
			handle = new Lock::LockHandle;
			pthread_mutex_init(&handle->mtx, NULL);
		}
		handle->key = k;
		handle->times = 0;
		_pool[k] = handle;
	}
	handle->times++;
	return std::move(Lock(handle, this));
}

void LockPool::recycle(Lock::LockHandle *handle) {
	L l(&_mtx);
	handle->times--;
	if(handle->times == 0) {
		_pool.erase(handle->key);
		handle->key = "";
		if(_cache.size() < _max) {
			_cache.push_back(handle);
		} else {
			pthread_mutex_destroy(&handle->mtx);
			delete handle;
		}
	}
}

Lock::~Lock() {
	if(_handle) {
		unlock();
		_pool->recycle(_handle);
	}
}

Lock::Lock(LockHandle *handle, LockPool *pool) {
	_handle = handle;
	_pool = pool;
}