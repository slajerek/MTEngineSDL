#include "CPool.h"
#include "DBG_Log.h"
#include <cstring>

// Create a new pool by defining the number of items to preallocate, and the
// size of each item.  For situations where you do not want the allocation
// pool increase in size, set bGrow to false.
CPool::CPool(const WORD nPoolSize, const size_t nItemSize, const bool bGrow) :
	m_pPrev(0),
	m_pNext(0),
	m_nPoolSize(nPoolSize),
	m_nItemSize(nItemSize),
	m_nTOS(0),
	m_bGrow(bGrow)
{
	//LOGD("CPool::CPool (1)");
	m_pPool = reinterpret_cast<BYTE*>(::operator new(m_nItemSize * m_nPoolSize));
	m_pFreeStack = new BYTE*[m_nPoolSize];
	m_pAvailable = m_pPool;
	m_pLast = m_pPool + m_nItemSize * m_nPoolSize;
#if defined(MT_POOL_GUARD)
	m_pSlotInUse = new BYTE[m_nPoolSize];
	memset(m_pSlotInUse, 0, m_nPoolSize);
#endif
	
	mutex = new CSlrMutex("CPool");
}

// Add a new pool to the end of the linked list, this can only be called from
// another CPool.
CPool::CPool(CPool* const pPrev) :
	m_pPrev(pPrev),
	m_pNext(0),
	m_nPoolSize(pPrev->m_nPoolSize),
	m_nItemSize(pPrev->m_nItemSize),
	m_nTOS(0),
	m_bGrow(true)
{
	//LOGD("CPool::CPool (2)");
	m_pPool = reinterpret_cast<BYTE*>(::operator new(m_nItemSize * m_nPoolSize));
	m_pFreeStack = new BYTE*[m_nPoolSize];
	m_pAvailable = m_pPool;
	m_pLast = m_pPool + m_nItemSize * m_nPoolSize;
#if defined(MT_POOL_GUARD)
	m_pSlotInUse = new BYTE[m_nPoolSize];
	memset(m_pSlotInUse, 0, m_nPoolSize);
#endif
	
	mutex = new CSlrMutex("CPool");
}

CPool::~CPool()
{
	//LOGD("~CPool()");
	delete m_pNext;
	delete m_pPool;
	delete [] m_pFreeStack;
#if defined(MT_POOL_GUARD)
	delete [] m_pSlotInUse;
#endif

	delete mutex;
}

#if defined(MT_POOL_GUARD)
// Both are called with the pool's mutex HELD and with pItem already known to
// lie inside [m_pPool, m_pLast).
void CPool::GuardOnNew(BYTE *pItem)
{
	m_pSlotInUse[(pItem - m_pPool) / m_nItemSize] = 1;
}

bool CPool::GuardOnDelete(BYTE *pItem)
{
	size_t offset = (size_t)(pItem - m_pPool);

	// A pointer into the middle of a slot is never something this pool handed
	// out. Deleting one would corrupt the free stack with an unaligned address
	// that a later New() would return as if it were an object.
	if (offset % m_nItemSize != 0)
	{
		LOGError("CPool::Delete: pointer %p is not on an item boundary (pool=%p itemSize=%d) -- NOT freed",
				 pItem, m_pPool, (int)m_nItemSize);
		return false;
	}

	BYTE *slot = &m_pSlotInUse[offset / m_nItemSize];
	if (*slot == 0)
	{
		// The bug this exists for. Pushing it again would put one slot on the
		// free stack twice, and the pool would then hand the SAME object to two
		// owners -- silent, and invisible to AddressSanitizer because no system
		// allocator is involved. Refusing the second free keeps the pool
		// consistent, so the run continues and the log names the culprit.
		LOGError("CPool::Delete: DOUBLE FREE of %p (pool=%p itemSize=%d) -- ignored, pool left consistent",
				 pItem, m_pPool, (int)m_nItemSize);
		return false;
	}

	*slot = 0;
	return true;
}
#endif

void* CPool::New(const size_t size)
{
//	LOGD("CPool::New: pool=%x size=%d", m_pPool, (u64)size);

	// If the item being requested is not the right size then use the generalised
	// new operator.  This will occur if the object is derived from the allocated
	// class.
	if (size != m_nItemSize)
	{
//		LOGError("size=%d != m_nItemSize=%d", size, m_nItemSize);
		return ::operator new(size);
	}

	mutex->Lock();

	// If there are any holes in the free stack then fill them up.
	if (m_nTOS == 0)
	{
		// If there is any space left in this pool then use it, otherwise move
		// on to the next in the linked list.
		if (m_pAvailable < m_pLast)
		{
			BYTE*	pReturn = m_pAvailable;
			m_pAvailable += m_nItemSize;
#if defined(MT_POOL_GUARD)
			GuardOnNew(pReturn);
#endif
			mutex->Unlock();
//			LOGD("CPool::New: pool=%x pReturn=%x", m_pPool, pReturn);
			return reinterpret_cast<void*>(pReturn);
		}
		else
		{
			// If there is another pool in the list then pass the request on to
			// it, otherwise try to create a new pool.
			if (m_pNext)
			{
				mutex->Unlock();
				return m_pNext->New(size);
			}
			else
			{
				if (m_bGrow)
				{
					m_pNext = new CPool(this);
					if (m_pNext)
					{
						mutex->Unlock();
						return m_pNext->New(size);
					}
				}
				// If we cannot allocate a new pool then return 0.
				mutex->Unlock();
				return 0;
			}
		}
	}
	else
	{
		// NOTE: the unlock used to happen BEFORE this line, so `--m_nTOS` and the
		// read of m_pFreeStack ran unlocked -- two threads could pop the same
		// slot and each treat it as their own object. Moved below the read.
		void *pReturn = reinterpret_cast<void*>(m_pFreeStack[--m_nTOS]);
#if defined(MT_POOL_GUARD)
		GuardOnNew(reinterpret_cast<BYTE*>(pReturn));
#endif
		mutex->Unlock();
//		LOGD("CPool::New: pool=%x pReturn=%x", m_pPool, pReturn);
		return pReturn;
	}
}

void CPool::Delete(void* pVoid)
{
//	LOGD("CPool::Delete: pool=%x pVoid=%x ", m_pPool, pVoid);
	
	mutex->Lock();
	if (pVoid)
	{
		BYTE*	pItem = reinterpret_cast<BYTE*>(pVoid);

		// Check if the item being deleted is within this pool's memory range.
		if (pItem < m_pPool || pItem >= m_pLast)
		{
			// If there is another pool in the list then try to delete from it,
			// otherwise call the generalised delete operator.
			if (m_pNext)
				m_pNext->Delete(pItem);
			else
				::operator delete(pVoid);
			
			mutex->Unlock();
		}
		else
		{
#if defined(MT_POOL_GUARD)
			if (!GuardOnDelete(pItem))
			{
				mutex->Unlock();
				return;
			}
#endif
			// Add a hole to the free stack.
			m_pFreeStack[m_nTOS++] = pItem;

			// If this pool is empty and it is the last in the linked list
			// then delete it and set the previous pool to the last.
			if (m_pPrev && !m_pNext &&
				static_cast<long>(m_nTOS * m_nItemSize) == m_pAvailable - m_pPool)
			{
				m_pPrev->m_pNext = 0;
				
				mutex->Unlock();
				delete this;
			}
			else
			{
				mutex->Unlock();
			}
		}
	}
	else
	{
		mutex->Unlock();
	}
}

// Reset all the pointers and indices, effectively deleting the allocated
// items without actually calling their destructors.  This function should
// be used with extreme caution since all sorts of horrible things can result
// from it's misuse.
void CPool::Purge()
{
	m_nTOS = 0;
	m_pAvailable = m_pPool;
#if defined(MT_POOL_GUARD)
	// Every slot is free again by definition. Without this the map would keep
	// saying "in use" for slots Purge just reclaimed, and the guard would miss
	// a stray free of one of them.
	memset(m_pSlotInUse, 0, m_nPoolSize);
#endif
	if (m_pNext)
		m_pNext->Purge();
}


