// macOS: png_load and png_save (image_io.h) through ImageIO. Any format it can
// read (the menus' JPEG too), with straight alpha as WIC gives it.
#include "image_io.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cstring>

bool png_load(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
        if (!source) return false;
        CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
        CFRelease(source);
        if (!image) return false;
        w = int(CGImageGetWidth(image));
        h = int(CGImageGetHeight(image));
        rgba.resize(size_t(w) * h * 4);
        CGImageAlphaInfo alpha = CGImageGetAlphaInfo(image);
        CGBitmapInfo order = CGImageGetBitmapInfo(image) & kCGBitmapByteOrderMask;
        bool ok = false;
        if (alpha == kCGImageAlphaLast && CGImageGetBitsPerPixel(image) == 32 && CGImageGetBitsPerComponent(image) == 8 &&
            (order == kCGBitmapByteOrderDefault || order == kCGBitmapByteOrder32Big)) {
            // Already straight RGBA: the bytes as they are (drawing them would premultiply)
            CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(image));
            if (data) {
                const uint8_t* src = CFDataGetBytePtr(data);
                size_t stride = CGImageGetBytesPerRow(image);
                for (int y = 0; y < h; y++) memcpy(&rgba[size_t(y) * w * 4], src + size_t(y) * stride, size_t(w) * 4);
                CFRelease(data);
                ok = true;
            }
        } else {
            // Its own colour space: the values stay as in the file, as with WIC
            CGColorSpaceRef space = CGImageGetColorSpace(image);
            if (!space || CGColorSpaceGetModel(space) != kCGColorSpaceModelRGB) space = nullptr;
            CGColorSpaceRef rgb = space ? CGColorSpaceRetain(space) : CGColorSpaceCreateDeviceRGB();
            CGContextRef c = CGBitmapContextCreate(rgba.data(), size_t(w), size_t(h), 8, size_t(w) * 4, rgb,
                                                   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            CGColorSpaceRelease(rgb);
            if (c) {
                CGContextSetBlendMode(c, kCGBlendModeCopy);
                CGContextDrawImage(c, CGRectMake(0, 0, w, h), image);
                CGContextRelease(c);
                for (size_t i = 0; i < rgba.size(); i += 4) {   // back to straight alpha
                    int a = rgba[i + 3];
                    if (a == 0 || a == 255) continue;
                    for (int k = 0; k < 3; k++) rgba[i + k] = uint8_t(std::min(255, (rgba[i + k] * 255 + a / 2) / a));
                }
                ok = true;
            }
        }
        CGImageRelease(image);
        return ok && w > 0 && h > 0;
    }
}

bool png_save(const std::string& path, int w, int h, const uint8_t* rgba) {
    @autoreleasepool {
        NSData* pixels = [NSData dataWithBytes:rgba length:size_t(w) * h * 4];
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
        CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
        CGImageRef image = CGImageCreate(size_t(w), size_t(h), 8, 32, size_t(w) * 4, rgb,
                                         kCGImageAlphaLast | kCGBitmapByteOrder32Big, provider, nullptr, false,
                                         kCGRenderingIntentDefault);
        CGColorSpaceRelease(rgb);
        CGDataProviderRelease(provider);
        if (!image) return false;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        CGImageDestinationRef dest = CGImageDestinationCreateWithURL((__bridge CFURLRef)url,
                                                                     (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
        bool ok = false;
        if (dest) {
            CGImageDestinationAddImage(dest, image, nullptr);
            ok = CGImageDestinationFinalize(dest);
            CFRelease(dest);
        }
        CGImageRelease(image);
        return ok;
    }
}
