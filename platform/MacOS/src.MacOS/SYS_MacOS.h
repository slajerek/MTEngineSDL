#ifndef _SYS_MACOS_H_
#define _SYS_MACOS_H_

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

class CSlrString;

NSString *FUN_ConvertCStringToNSString(const char *str);
NSString *FUN_ConvertCSlrStringToNSString(CSlrString *str);
CSlrString *FUN_ConvertNSStringToCSlrString(NSString *nsstr);

NSString *MACOS_GetPathForResource(char *fileNameX);
float MACOS_GetBackingScaleFactor(int screen);

#endif
