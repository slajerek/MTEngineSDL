#ifndef CCOMBINATIONS_H_
#define CCOMBINATIONS_H_

// the algorithm is from Applied Combinatorics, by Alan Tucker

template<class T>
class CCombinations
{
public:
	T **inArray;
	int n, m;
	bool hasMore;
	int *index;

	CCombinations(T **inArray, int arrayLen, int m)
	{
		this->inArray = inArray;
		this->n = arrayLen;
		this->m = m;
		
		if( n < 0 )
		{
			LOGError( "n, the number of items, must be greater than 0, n=%d", n);
		}
		else if( n < m )
		{
			LOGError( "n, the number of items, must be >= m, the number selected, n=%d m=%d", n, m);
		}
		else if( m < 0 )
		{
			LOGError( "m, the number of selected items, must be >= 0, m=%d", m);
		}
		else
		{
			this->index = new int[n];
			for (int i = 0; i < m; i++)
				this->index[i] = i;
			
			this->hasMore = true;
		}
	};
	
	~CCombinations()
	{
		delete [] this->index;
	};
	
	bool hasMoreElements()
	{
		return hasMore;
	};
	
	void moveIndex()
	{
		int i = this->rightmostIndexBelowMax();
		if (i >= 0)
        {    
            index[i] = index[i] + 1; 
            for (int j = i + 1; j < m; j++)
                index[j] = index[j-1] + 1;
        }
        else
            hasMore = false;
	};
	
	T** nextElement()
	{
		if (!hasMore)
			return NULL;
		
		T** out = new T*[m];
		for (int i = 0; i < m; i++)
			out[i] = inArray[index[i]];
		
		moveIndex();
		return out;
	};
	
	int rightmostIndexBelowMax()
    {
        for (int i = m - 1; i >= 0; i--)
        {
            if (index[i] < n - m + i)
            	return i;
        }
        return -1;
    };
	
};

#endif /*CCOMBINATIONS_H_*/
