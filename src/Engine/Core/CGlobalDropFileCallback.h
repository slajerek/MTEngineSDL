#ifndef _CGlobalDropFileCallback_h_
#define _CGlobalDropFileCallback_h_

class CGlobalDropFileCallback
{
public:
	virtual void GlobalDropFileCallback(char *filePath, bool consumedByView);
};

#endif
