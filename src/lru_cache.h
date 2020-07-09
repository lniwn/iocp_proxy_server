#pragma once
#include <shared_mutex>

template<typename Key, typename Data>
class LRUCache
{
public:
	using CacheList = std::list<std::pair<Key, Data>>;
	using CacheListIter = typename CacheList::iterator;
	using CacheMap = std::unordered_map<Key, CacheListIter>;
	using CacheMapIter = typename CacheMap::iterator;

public:
	explicit LRUCache(unsigned long maxSize)
		:m_maxSize(maxSize)
	{
		assert(m_maxSize > 0);
		m_curSize = 0;
	}

	~LRUCache()
	{

	}

	inline unsigned long Size() const { return m_curSize; }

	inline unsigned long MaxSize() const { return m_maxSize; }

	void Clear()
	{
		std::unique_lock<std::shared_mutex> _(m_guard);
		m_storage.clear();
		m_index.clear();
		m_curSize = 0;
	}

	inline bool Exists(const Key& k) const {
		std::shared_lock<std::shared_mutex> _(m_guard);
		return m_index.find(k) != m_index.end();
	}

	void Remove(const Key& k)
	{
		std::unique_lock<std::shared_mutex> _(m_guard);
		auto iter = m_index.find(k);
		if (iter != m_index.end())
		{
			remove(iter);
		}
	}

	void RemoveIf(std::function<bool(const Key&, const Data&)> binaryPredicate)
	{
		std::unique_lock<std::shared_mutex> _(m_guard);
		for (auto it = m_storage.begin(); it != m_storage.end(); it++)
		{
			if (binaryPredicate(it->first, it->second))
			{
				m_index.erase(it->first);
				it = m_storage.erase(it);
				--m_curSize;
			}
		}
	}

	inline void Touch(const Key& k)
	{
		std::unique_lock<std::shared_mutex> _(m_guard);
		touch(k);
	}

	bool Fetch(const Key& k, Data* pData, bool touchData = true)
	{
		if (touchData)
		{
			std::unique_lock<std::shared_mutex> _(m_guard);
			auto iter = m_index.find(k);
			if (iter == m_index.end())
			{
				return false;
			}
			touch(k);
			*pData = iter->second->second;
			return true;
		}
		else
		{
			std::shared_lock<std::shared_mutex> _(m_guard);
			auto iter = m_index.find(k);
			if (iter == m_index.end())
			{
				return false;
			}
			*pData = iter->second->second;
			return true;
		}
	}

	void Insert(const Key& k, const Data& d)
	{
		std::unique_lock<std::shared_mutex> _(m_guard);
		auto miter = m_index.find(k);
		if (miter != m_index.end())
		{
			remove(miter);
		}

		m_storage.push_back(std::make_pair(k, d));
		auto liter = std::prev(m_storage.end());

		m_index.insert(std::make_pair(k, liter));
		++m_curSize;

		while (m_curSize > m_maxSize)
		{
			remove(m_storage.begin()->first);
		}
	}

private:
	void remove(const CacheMapIter& iter)
	{
		assert(m_curSize > 0);
		m_storage.erase(iter->second);
		m_index.erase(iter);
		--m_curSize;
	}

	void remove(const Key& k)
	{
		remove(m_index.find(k));
	}

	CacheMapIter touch(const Key& k)
	{
		auto iter = m_index.find(k);
		if (iter == m_index.end())
		{
			return iter;
		}
		// 如果元素已经在最新位置则不需要拼接
		if (std::prev(m_storage.end()) == iter->second)
		{
			return iter;
		}

		m_storage.splice(m_storage.end(), m_storage, iter->second);
		return iter;
	}

private:
	CacheList m_storage; // 队列，新元素添加到队尾
	CacheMap m_index;
	const unsigned long m_maxSize;
	std::atomic_ulong m_curSize;
	std::shared_mutex m_guard;
};
