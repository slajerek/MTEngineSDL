#include "SYS_MacOS.h"
#include "CSlrString.h"

NSString *FUN_ConvertCStringToNSString(const char *str)
{
	NSString *nsstr = [[NSString alloc] initWithCString:str encoding:NSUTF8StringEncoding];
	return nsstr;
}

NSString *FUN_ConvertCSlrStringToNSString(CSlrString *str)
{
	u32 len;
	u16 *buffer = str->GetUTF16(&len);
	
	NSData *data = [[NSData alloc] initWithBytesNoCopy:buffer length:len*2 freeWhenDone:YES];
	NSString *nsstr = [[NSString alloc] initWithData:data encoding:NSUTF16LittleEndianStringEncoding];
//	[data release];
	
	return nsstr;
}

CSlrString *FUN_ConvertNSStringToCSlrString(NSString *nsstr)
{
	NSData *data = [nsstr dataUsingEncoding:NSUTF16StringEncoding];
	
	u8 *buffer = (u8*)[data bytes];
	u32 len = [data length];
	CSlrString *retStr = new CSlrString(buffer, len);
	
	return retStr;
}

NSString *MACOS_GetPathForResource(char *fileNameX)
{
	char resNameNoPath[2048];
	int i = strlen(fileNameX)-1;
	
	char fileNamePath[MAX_STRING_LENGTH];
	
	bool isSlash = false;
	for(u16 j = 0; j < i; j++)
	{
		if (fileNameX[j] == '/')
		{
			isSlash = true;
			break;
		}
	}
	
	if (isSlash)
	{
		strcpy(fileNamePath, fileNameX);
	}
	else
	{
		sprintf(fileNamePath, "/%s", fileNameX);
	}
	
	for (  ; i >= 0; i--)
	{
		if (fileNamePath[i] == '/')
			break;
	}
	
	int j = 0;
	while(true)
	{
		if (fileNamePath[i] == '.')
		{
			resNameNoPath[j] = '\0';
			break;
		}
		resNameNoPath[j] = fileNamePath[i];
		if (fileNamePath[i] == '\0')
			break;
		j++;
		i++;
	}
	
	char ext[16] = {0};
	if (fileNamePath[i] == '.')
	{
		i++;
		j = 0;
		while(true)
		{
			ext[j] = fileNamePath[i];
			if (fileNamePath[i] == '\0')
				break;
			j++;
			i++;
		}
	}
	else
	{
		ext[0] = '\0';
	}
	
	NSString *nsFileName = [NSString stringWithCString:resNameNoPath encoding:NSASCIIStringEncoding];
	NSString *nsExtName = [NSString stringWithCString:ext encoding:NSASCIIStringEncoding];
	
	// iOS3.2
	NSString *fileNameNoSlash = [nsFileName stringByReplacingOccurrencesOfString:@"/" withString:@""];
	
//	NSLog(@"fileNameNoSlash=%@", fileNameNoSlash);
	
	//NSString *path = [[NSBundle mainBundle] pathForResource:nsFileName ofType:nsExtName];
	NSString *path = [[NSBundle mainBundle] pathForResource:fileNameNoSlash ofType:nsExtName inDirectory:@""];
	
	return path;
}

// workaround for SDL 2.0.10 bug: https://bugzilla.libsdl.org/show_bug.cgi?id=4856
// based on: https://github.com/ocornut/imgui/commit/a843af4306e0d786fec5394bba07fd5067384661
//
// `screen` IS AN INDEX INTO [NSScreen screens]. It is NOT an SDL display ID.
// Under SDL2 the two were interchangeable in practice because
// SDL_GetWindowDisplayIndex also returned an index; under SDL3
// SDL_GetDisplayForWindow returns an OPAQUE HANDLE that typically starts at 1,
// so passing it here reads the wrong screen or falls off the end and silently
// returns 1.0f. That happened, and it cost the shaders their Retina scale.
//
// Currently UNUSED: every caller moved to SDL_GetWindowPixelDensity, which is
// the same quantity, follows the window across screens, and needs no index.
// Kept because it is a legitimate platform primitive -- but if you reach for
// it, feed it an NSScreen index or do not use it.
float MACOS_GetBackingScaleFactor(int screen)
{
	if (screen >= [NSScreen screens].count)
		return 1.f;
	return static_cast<float>([[NSScreen screens][static_cast<NSUInteger>(screen)] backingScaleFactor]);
}
