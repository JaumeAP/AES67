//
// Controller-Bridging-Header.h
// AES67 Controller
//
// What Swift sees of the driver's network half: the discovery bridge, and
// nothing else. No Core Audio, no plug-in, no audio path -- this application
// routes other people's devices and does not need an audio device of its own.
//

#import "NetworkEngine/Discovery/DiscoveryBridge.h"
