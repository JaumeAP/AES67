//
// HALValidateAudioAccess.mm
// Microphone (TCC) authorization query for HALValidate.
//
// Opening a Core Audio device that has input streams goes through TCC. From a
// plain CLI binary with no decision recorded for its parent terminal, the
// AudioDeviceCreateIOProcID call blocks instead of prompting, and the process
// hangs with no output. Querying the status first is what lets the validator
// skip the IO section with an explanation rather than freeze.
//

#import <AVFoundation/AVFoundation.h>

extern "C" int HALValidateMicrophoneAuthorization(void) {
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    switch (status) {
        case AVAuthorizationStatusAuthorized:
            return 1;
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted:
            return 2;
        case AVAuthorizationStatusNotDetermined:
        default:
            return 0;
    }
}
