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
float MACOS_GetBackingScaleFactor(int screen)
{
	if (screen >= [NSScreen screens].count)
		return 1.f;
	return static_cast<float>([[NSScreen screens][static_cast<NSUInteger>(screen)] backingScaleFactor]);
}
