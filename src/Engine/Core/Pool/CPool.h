#ifndef __POOL_H__
#define __POOL_H__

#include "SYS_Defs.h"
#include "SYS_Threading.h"
#include "DBG_Log.h"

// DOUBLE-FREE / STRAY-FREE GUARD, development builds only.
//
// CPool::Delete() recycles memory itself instead of returning it to the system
// allocator, so AddressSanitizer cannot see anything that happens to a pooled
// object: a double delete, or a use after delete, is simply invisible. That is
// not hypothetical. On 2026-09-09 a CSlrString was deleted twice (a global path
// string returned by CRecentlyOpenedFiles::GetCurrentOpenedFolder() and then
// deleted by its caller), the pool handed the same object out to two live
// strings, and they corrupted each other's pooled character vector. The visible
// result was a heap-buffer-overflow spraying UTF-16 text through unrelated
// objects and crashing the app in three different threads, tens of tests away
// from the actual bug. It cost three debugging passes and was only found by
// switching the pool OFF so ASan could see the object again.
//
// This guard turns that whole class of bug into one log line at the moment it
// happens. It is a per-item byte, checked in O(1), so the cost is a bounds
// compare and a byte store on each New/Delete.
//
// Gated on MT_DEBUG_LOGS, which the BUILD decides (1 = development,
// 0 = ./build-<os>.sh --prod). Final builds compile it out entirely.
#if MT_DEBUG_LOGS
#define MT_POOL_GUARD 1
#endif

#define WORD u16
#define BYTE u8

// Fast Allocation Pool
// Submitted by Jon Creighton

// taken from: http://www.flipcode.com/archives/Fast_Allocation_Pool.shtml
// http://www.flipcode.com/archives/toolbox.shtml

// CPool:
// An optimised way of performing dynamic memory allocation, using a
// preallocated pool which grows in large blocks as more objects are required.

// To use it, add a static member variable to the class definition:
//	 static CPool m_pool;
// Which has to be initialised in the class's .cpp file as:
//   CPool CItem::m_pool(1000, sizeof(CItem));
//     Where 1000 is the initial number of items in the pool.

// Then add overrides for the new and delete operator of your class:
//	 static void* operator new(const size_t size)
//   {
//       return m_pool.New(size);
//   }
//	 static void operator delete(void* pObject);
//   {
//       m_pool.Delete(pObject);
//   }

// This class was originally implemented as a template but I decided that it
// wasn't necessary since the new and delete operators always use void* anyway.

class CPool
{
public:
	CPool(const WORD nPoolSize, const size_t nItemSize, const bool bGrow = true);
	~CPool();

	void* New(const size_t size);
	void Delete(void* pVoid);
	void Purge();

private:
	CPool(CPool* const pPrev);

	// New allocation pools are created and inserted into a doubly-linked list.
	CPool* const	m_pPrev;
	CPool*			m_pNext;

	const WORD		m_nPoolSize;	// Maximum number of items in the pool.
	const size_t	m_nItemSize;	// The size of the contained item.
	BYTE*			m_pAvailable;	// Next available item.
	BYTE*			m_pLast;		// End of the pool.
	WORD			m_nTOS;			// Top of the free stack.
	bool			m_bGrow;		// True if the pool is allowed to grow.

	BYTE*			m_pPool;		// The allocation pool of items.
	BYTE**			m_pFreeStack;	// The stack of deleted items.

#if defined(MT_POOL_GUARD)
	// One byte per slot: 1 while the slot is handed out, 0 while it is free.
	// Indexed by (pItem - m_pPool) / m_nItemSize, so the check is O(1) rather
	// than a scan of the free stack -- which for the 65535-slot strings pool
	// would have made every delete quadratic.
	BYTE*			m_pSlotInUse;
	void			GuardOnNew(BYTE *pItem);
	// Returns false when the item must NOT be pushed onto the free stack.
	bool			GuardOnDelete(BYTE *pItem);
#endif
	
	CSlrMutex *mutex;
};

#endif
