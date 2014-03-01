#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOMessage.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include <IOKit/hidsystem/IOHIDParameter.h>

#include <ApplicationServices/ApplicationServices.h>

/* */
static IONotificationPortRef gNotifyPort = NULL;
static io_iterator_t gAddedIter = 0;
static CGPoint point0 = {0, 0};

/* */
typedef enum CalibrationState {
    kCalibrationStateInactive = 0,
    kCalibrationStateTopLeft,
    kCalibrationStateTopRight,
    kCalibrationStateBottomRight,
    kCalibrationStateBottomLeft
} CalibrationState;

typedef struct HIDData {
    io_object_t	notification;
    IOHIDDeviceInterface122 **hidDeviceInterface;
    IOHIDQueueInterface **hidQueueInterface;
    CFDictionaryRef hidElementDictionary;
    CFRunLoopSourceRef eventSource;
    CalibrationState state;
    SInt32 minx;
    SInt32 maxx;
    SInt32 miny;
    SInt32 maxy;
    UInt8 buffer[256];
} HIDData;

typedef HIDData* HIDDataRef;

typedef struct HIDElement {
    SInt32 currentValue;
    SInt32 usagePage;
    SInt32 usage;
    IOHIDElementType type;
    IOHIDElementCookie cookie;
    HIDDataRef owner;
}HIDElement;

typedef HIDElement* HIDElementRef;

/* */
static void find_device ();
static void init_device (void *refCon, io_iterator_t iterator);
static void device_release (void *refCon, io_service_t service,
							natural_t messageType, void *messageArgument);
static void interrupt_callback (void *target, IOReturn result, void *refcon,
								void *sender, uint32_t bufferSize);

/* */
int main (int argc, const char * argv[]) {
	find_device();
	
    CFRunLoopRun();
	
	return 0;
}

void find_device () {
    CFMutableDictionaryRef matchingDict;
    CFNumberRef refUsage;
    CFNumberRef refUsagePageKey;
    SInt32 usage = 1;        /* mouse */
    SInt32 usagePageKey = 2; /* mouse */
    mach_port_t	masterPort;
    kern_return_t kr;
    
    kr = IOMasterPort (bootstrap_port, &masterPort);
    if (kr || !masterPort) {
        return;
	}
	
    gNotifyPort = IONotificationPortCreate (masterPort);
    CFRunLoopAddSource (CFRunLoopGetCurrent (), IONotificationPortGetRunLoopSource (gNotifyPort), kCFRunLoopDefaultMode);
	
    matchingDict = IOServiceMatching ("IOHIDDevice");
    
    if (!matchingDict) {
		return;
	}
	
    refUsage = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &usage);
    refUsagePageKey = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &usagePageKey);
    
    CFDictionarySetValue (matchingDict, CFSTR (kIOHIDPrimaryUsageKey), refUsagePageKey);
    CFDictionarySetValue (matchingDict, CFSTR (kIOHIDPrimaryUsagePageKey), refUsage);
    
    CFRelease (refUsage);
    CFRelease (refUsagePageKey);
	
    kr = IOServiceAddMatchingNotification (gNotifyPort, kIOFirstMatchNotification, matchingDict, init_device, NULL, &gAddedIter);
	
    if (kr != kIOReturnSuccess) {
        return;
	}
	
    init_device (NULL, gAddedIter);
}

void init_device (void *refCon, io_iterator_t iterator) {
    io_object_t hidDevice = 0;
    IOCFPlugInInterface **plugInInterface = NULL;
    IOHIDDeviceInterface122 **hidDeviceInterface = NULL;
    HRESULT result = S_FALSE;
    HIDDataRef hidDataRef = NULL;
    IOReturn kr;
    SInt32 score;
	
    while (hidDevice = IOIteratorNext (iterator)) {
        kr = IOCreatePlugInInterfaceForService (hidDevice,
												kIOHIDDeviceUserClientTypeID,
												kIOCFPlugInInterfaceID,
												&plugInInterface, &score);
		
        if (kr != kIOReturnSuccess) {
            goto HIDDEVICEADDED_NONPLUGIN_CLEANUP;
		}
		
        result = (*plugInInterface)->QueryInterface (plugInInterface,
													 CFUUIDGetUUIDBytes
													 (kIOHIDDeviceInterfaceID),
													 (LPVOID *)&hidDeviceInterface);
		
        if ((result == S_OK) && hidDeviceInterface) {
            hidDataRef = malloc (sizeof (HIDData));
            bzero (hidDataRef, sizeof (HIDData));
            
            hidDataRef->hidDeviceInterface = hidDeviceInterface;
			
            result = (*(hidDeviceInterface))->open
			(hidDataRef->hidDeviceInterface, 0);
            result = (*(hidDeviceInterface))->createAsyncEventSource
			(hidDataRef->hidDeviceInterface, &hidDataRef->eventSource);
            result = (*(hidDeviceInterface))->setInterruptReportHandlerCallback
			(hidDataRef->hidDeviceInterface, hidDataRef->buffer,
			 sizeof(hidDataRef->buffer), &interrupt_callback, NULL,
			 hidDataRef);
			
            CFRunLoopAddSource (CFRunLoopGetCurrent (), hidDataRef->eventSource,
								kCFRunLoopDefaultMode);
			
            IOServiceAddInterestNotification (gNotifyPort, hidDevice,
											  kIOGeneralInterest,
											  device_release, hidDataRef,
											  &(hidDataRef->notification));
			
            goto HIDDEVICEADDED_CLEANUP;
        }
		
        if (hidDeviceInterface) {
            (*hidDeviceInterface)->Release(hidDeviceInterface);
            hidDeviceInterface = NULL;
        }
        
        if (hidDataRef) {
            free ( hidDataRef );
		}
		
	HIDDEVICEADDED_CLEANUP:
        (*plugInInterface)->Release(plugInInterface);
        
	HIDDEVICEADDED_NONPLUGIN_CLEANUP:
        IOObjectRelease(hidDevice);
    }
}

void device_release (void *refCon, io_service_t service, natural_t messageType,
					 void *messageArgument) {
    kern_return_t kr;
    HIDDataRef hidDataRef = (HIDDataRef) refCon;
	
    if ((hidDataRef != NULL) && (messageType == kIOMessageServiceIsTerminated)) {
        if (hidDataRef->hidQueueInterface != NULL) {
            kr = (*(hidDataRef->hidQueueInterface))->stop
			((hidDataRef->hidQueueInterface));
            kr = (*(hidDataRef->hidQueueInterface))->dispose
			((hidDataRef->hidQueueInterface));
            kr = (*(hidDataRef->hidQueueInterface))->Release
			(hidDataRef->hidQueueInterface);
            hidDataRef->hidQueueInterface = NULL;
        }
		
        if (hidDataRef->hidDeviceInterface != NULL) {
            kr = (*(hidDataRef->hidDeviceInterface))->close
			(hidDataRef->hidDeviceInterface);
            kr = (*(hidDataRef->hidDeviceInterface))->Release
			(hidDataRef->hidDeviceInterface);
            hidDataRef->hidDeviceInterface = NULL;
        }
        
        if (hidDataRef->notification) {
            kr = IOObjectRelease(hidDataRef->notification);
            hidDataRef->notification = 0;
        }
    }
}

void interrupt_callback (void *target, IOReturn result, void *refcon,
                         void *sender, uint32_t bufferSize) {
    static int interrupt_counter = 0;
    HIDDataRef hidDataRef = (HIDDataRef) refcon;
    char hw_x = SCHAR_MAX, hw_y = SCHAR_MAX; /* hardware coordinates, received from mouse */
    int sw_x = INT_MAX, sw_y = INT_MAX;      /* software coordinates, grabbed from system mouse location */

    if (!hidDataRef) {
        return;
    }

    if (bufferSize < 4) {
        return;
    }

    CGEventRef event = CGEventCreate (NULL);
    CGPoint point = CGEventGetLocation(event);

    sw_x = point.x - point0.x;
    sw_y = point.y - point0.y;
    point0.x = point.x;
    point0.y = point.y;

    if (interrupt_counter > 0) {
        printf("         sw: %3i  x %3i\n", sw_x, sw_y);
    }

    hw_x = (char) hidDataRef->buffer[1];
    hw_y = (char) hidDataRef->buffer[2];

    printf("hw: %3i  x %3i", hw_x, hw_y);

    interrupt_counter++;
}