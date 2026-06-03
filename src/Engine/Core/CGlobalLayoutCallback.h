#ifndef _LAYOUT_CALLBACK_
#define _LAYOUT_CALLBACK_

class CLayoutData;

// Fires once per layout (workspace) restore, before any view's
// DeserializeLayout runs. Use it to reset global state that views would
// otherwise leak across workspaces.
class CGlobalLayoutCallback
{
public:
	virtual void GlobalLayoutWillDeserialize(CLayoutData *layout);
};

#endif
