#include <iostream>
#include <vector>
#include <string.h>
#include <math.h>
#include <chrono>
#include <time.h>

#include <MiIni.h>
#include <circular_queue.h>

#include <signal.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>

using namespace std::chrono;

struct Monitor
{
    int x, y, w, h;

    bool contains(int xpos, int ypos, int margin = 0)
    {
        return (xpos >= x + margin && xpos <= (x + w - margin) && ypos >= y + margin && ypos <= (y + h - margin));
    }
};

struct PtrEntry
{
    PtrEntry(int x, int y, float speed) : x(x),
                                          y(y),
                                          speed(speed)
        {
            moveTime = high_resolution_clock::now();
        }

    int x, y;
    float speed;
    time_point<high_resolution_clock> moveTime;
};

struct PassConfig
{
    bool always;
    //float strength;
    //float slowerThan, fasterThan;
    duration<float> maxDelay, minDelay, baseDelay;
    duration<float> returnBefore;
    //duration<float> predict;
};

/*
Config variables
*/
MiIni config;
int cfgPtrInputsToRemember = 10;
duration<float> cfgPtrRememberForSeconds = 0.2s;
float cfgResistanceSlowdownExponent;
float cfgResistanceSpeedupExponent;
float cfgResistanceConstSpeedExponent;
float cfgPassthroughSmoothingFactor;
//duration<float> cfgPtrPredictSeconds = 0.2s;
PassConfig cfgEdgePass, cfgCornerPass;
float cfgCornerSizeFactor;
int cfgResistanceMargins;

/*
Display variables
*/
Display *display;// our display
Window rootWindow;// root wnd of our display
std::vector<Monitor> monitors;
Monitor *currentMonitor;// the monitor in which the pointer is

/*
Resistance calculation variables
*/
circular_queue<PtrEntry> ptrMemory;// ptr positions and data
bool onEdge;// are we on edge rn
time_point<high_resolution_clock> touchedEdgeTime;// the time point when we touched the edge, to detect delay
time_point<high_resolution_clock> brokeFromTime;// the time point when we last broke from a monitor
Monitor *brokeFromMonitor;// the monitor we passed FROM last time. Useful for returning to previous monitor when we miss a button or smthng.

void loadConfig()
{
    std::string homepath = getenv("HOME");
    config.open(homepath+"/.config/stick-cursor-to-screen", false);

    cfgPtrInputsToRemember = config.get("Movement Calculation", "NoInputsToRemember", 50);
    cfgPtrRememberForSeconds = (duration<float>)config.get("Movement Calculation", "RememberForSeconds", 0.15);
    cfgResistanceSlowdownExponent = config.get("Movement Calculation", "ResistanceSlowdownExponent", 4.0);
    cfgResistanceSpeedupExponent = config.get("Movement Calculation", "ResistanceSpeedupExponent", 1.0);
    cfgResistanceConstSpeedExponent = config.get("Movement Calculation", "ResistanceConstantSpeedExponent", 0.1);
    cfgPassthroughSmoothingFactor = config.get("Movement Calculation", "PassthroughSmoothingFactor", 0.05);

    cfgCornerSizeFactor = config.get("Screen", "CornerSizeFactor", 0.1f);
    cfgResistanceMargins = config.get("Screen", "ResistanceMargins", 1);

    cfgEdgePass.always = config.get("Edge Passthrough", "AllowAlways", false);
    //cfgEdgePass.strength = config.get("Edge Passthrough", "Strength", 2000.0f);
    cfgEdgePass.baseDelay = (duration<float>)config.get("Edge Passthrough", "BaseDelayOfSeconds", 0.4);
    cfgEdgePass.maxDelay = (duration<float>)config.get("Edge Passthrough", "MaxDelayOfSeconds", 0.6);
    cfgEdgePass.minDelay = (duration<float>)config.get("Edge Passthrough", "MinDelayOfSeconds", 0.0);
    cfgEdgePass.returnBefore = (duration<float>)config.get("Edge Passthrough", "FreelyReturnBeforeSeconds", 1);

    cfgCornerPass.always = config.get("Corner Passthrough", "AllowAlways", false);
    //cfgCornerPass.strength = config.get("Corner Passthrough", "Strength", 4000.0f);
    cfgCornerPass.baseDelay = (duration<float>)config.get("Corner Passthrough", "BaseDelayOfSeconds", 0.7);
    cfgCornerPass.maxDelay = (duration<float>)config.get("Corner Passthrough", "MaxDelayOfSeconds", 1);
    cfgCornerPass.minDelay = (duration<float>)config.get("Corner Passthrough", "MinDelayOfSeconds", 0.0);
    cfgCornerPass.returnBefore = (duration<float>)config.get("Corner Passthrough", "FreelyReturnBeforeSeconds", 1);


    /*cfgEdgePass.always = config.get("Edge Passthrough", "AllowAlways", false);
    cfgEdgePass.slowerThan = config.get("Edge Passthrough", "WhenSlowerThanUnitsPerSec", 300);
    cfgEdgePass.fasterThan = config.get("Edge Passthrough", "WhenFasterThanUnitsPerSec", 6500);
    cfgEdgePass.maxDelay = (duration<float>)config.get("Edge Passthrough", "AfterDelayOfSeconds", 0.6);
    cfgEdgePass.returnBefore = (duration<float>)config.get("Edge Passthrough", "ReturnBeforeSeconds", 1.5);
    cfgEdgePass.predict = (duration<float>)config.get("Edge Passthrough", "PredictSeconds", 0.05);

    cfgCornerPass.always = config.get("Corner Passthrough", "AllowAlways", false);
    cfgCornerPass.slowerThan = config.get("Corner Passthrough", "WhenSlowerThanUnitsPerSec", 50);
    cfgCornerPass.fasterThan = config.get("Corner Passthrough", "WhenFasterThanUnitsPerSec", 13000);
    cfgCornerPass.maxDelay = (duration<float>)config.get("Corner Passthrough", "AfterDelayOfSeconds", 1.2);
    cfgCornerPass.returnBefore = (duration<float>)config.get("Corner Passthrough", "ReturnBeforeSeconds", 1.5);
    cfgCornerPass.predict = (duration<float>)config.get("Corner Passthrough", "PredictSeconds", 0.05);*/

    config.sync();// In case the config didn't exist before
}

Monitor* getCurrentMonitor(int x, int y)
{
    Monitor *monitor = nullptr;
    for (auto& m : monitors)
    {
        if (m.contains(x, y))
        {
            monitor = &m;
        }
    }
    return monitor;
}

void updateMonitorList()
{
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(display, rootWindow);

    monitors.clear();

    // CRTC seems to be a monitor assigned to a rectangle of this Screen
    for( int j = 0; j < res->ncrtc; j++ )
    {
        XRRCrtcInfo *crtc_info = XRRGetCrtcInfo( display, res, res->crtcs[j] );
        if(crtc_info->noutput)
        {
            monitors.push_back({crtc_info->x,
                                crtc_info->y,
                                crtc_info->width,
                                crtc_info->height});
            printf("Found monitor:%3i x:%5i y:%5i w:%5i h:%5i\n",
                   j,
                   crtc_info->x,
                   crtc_info->y,
                   crtc_info->width,
                   crtc_info->height);
        }
        XFree(crtc_info);
    }
    XFree(res);

    // Reset pointer position info
    Window childDummy, parentDummy;
    int root_x, root_y, win_x, win_y;
    unsigned int maskDummy;
    XQueryPointer(display, rootWindow, &rootWindow, &childDummy,
        &root_x, &root_y, &win_x, &win_y, &maskDummy);

    ptrMemory.clear();
    for (int i = 0; i < cfgPtrInputsToRemember; i++)
        ptrMemory.emplace_back(root_x, root_y, 0);

    // Find the monitor on which we are rn
    currentMonitor = getCurrentMonitor(root_x, root_y);
    brokeFromMonitor = nullptr;
    onEdge = false;
}

void movePointer(int x, int y, Window confineWindow = None)
{
    //XGrabPointer(display, rootWindow, false, PointerMotionMask, GrabModeAsync,
    //             GrabModeAsync, confineWindow, None, CurrentTime);
    XWarpPointer(display, None, rootWindow, 0, 0, 0, 0, x, y);
    //XUngrabPointer(display, CurrentTime);
}

Window prevPointerWindow;// the window pointer was in last call. Useful for confining the pointer to prevent flicker
//float resistanceOvercame;
void pointerMoved(int x, int y, double dx, double dy, Window pointerWindow = None)
{
    // Remember the state
    PtrEntry &prev = ptrMemory[ptrMemory.size() - 1];// previous pointer state
    PtrEntry current(x, y, 0.0f);// current pointer state
    duration<float> secondsElapsed = current.moveTime - prev.moveTime;
    //int dx = x - ptrMemory[ptrMemory.size()-1].x;
    //int dy = y - ptrMemory[ptrMemory.size()-1].y;

    // calc speed
    float dis = std::sqrt(dx * dx + dy * dy);
    if (secondsElapsed.count() != 0)
    {
        current.speed = dis;
        //dis / secondsElapsed.count();
    }
    ptrMemory.pop_front();// remove old pointer data
    ptrMemory.push_back(current);// add new pointer data

    // Calc 2 average speeds to determine if we are accelerating or slowing down, and use the difference in further calcs
    float speed1 = 0.0f, speed2 = 0.0f;

    for (int i = 0; i < ptrMemory.size()-1; i++)
    {
        auto timeDiff = current.moveTime - ptrMemory[i].moveTime;

        if (timeDiff <= cfgPtrRememberForSeconds)
        {
            speed1 = ptrMemory[i].speed;
            break;
        }
    }
    speed2 = current.speed;

    // Calc resistance factor for making it harder to pass
    float resistanceFactor;
    if (speed1 > 0 && speed2 > 0)
    {
        // If we are slowing down, resistance must be higher (prolly trying to hit a button)
        resistanceFactor = speed1 / speed2;

        if (speed1 > speed2)
            resistanceFactor = std::pow(resistanceFactor, cfgResistanceSlowdownExponent);
        else
            resistanceFactor = std::pow(resistanceFactor, cfgResistanceSpeedupExponent);

        resistanceFactor *= std::pow(std::abs(speed1 - speed2) / std::max(speed1, speed2), cfgResistanceConstSpeedExponent);
        resistanceFactor = (resistanceFactor - cfgPassthroughSmoothingFactor) / (1.0 - cfgPassthroughSmoothingFactor);
    }
    else
        resistanceFactor = 1;

    //printf("Current: %9.2f, first: %9.2f, second: %9.2f, Res: %4.2f\n", dis, speed1, speed2, resistanceFactor);
    
    // Do nothing if we are outside any monitor
    if (currentMonitor)
    {
        // If the pointer tries to exit the monitor
        if (!currentMonitor->contains(x, y, cfgResistanceMargins+1))
        {
            Monitor *newMonitor = getCurrentMonitor(x, y);
            PassConfig *passCfg;
            bool pass;

            // Corner or edge? 
            bool onHorCorner = (x < currentMonitor->x + currentMonitor->w * cfgCornerSizeFactor) ||
                             (x > currentMonitor->x + currentMonitor->w * (1.0f - cfgCornerSizeFactor));
            bool onVerCorner = (y < currentMonitor->y + currentMonitor->h * cfgCornerSizeFactor) ||
                             (y > currentMonitor->y + currentMonitor->h * (1.0f - cfgCornerSizeFactor));

            if (onHorCorner && onVerCorner)
                passCfg = &cfgCornerPass;
            else
                passCfg = &cfgEdgePass;

            // Should we let the cursor pass?
            if (passCfg->always ||
                (newMonitor == brokeFromMonitor && (current.moveTime - brokeFromTime) < passCfg->returnBefore))
            {
                pass = true;
            }
            else
            {
                if (!onEdge)
                {
                    onEdge = true;
                    touchedEdgeTime = current.moveTime;
                    //resistanceOvercame = 0.0;
                }

                //resistanceOvercame += dis;
                auto adjustedDelay = (
                    std::max(
                        std::min(
                            passCfg->baseDelay * resistanceFactor,
                            passCfg->maxDelay),
                        passCfg->minDelay)
                    );

                if ((current.moveTime - touchedEdgeTime) > adjustedDelay) //resistanceOvercame >= passCfg->strength*resistanceFactor ||
                {
                    pass = true;
                }
                else
                {
                    pass = false;
                }
                
            }

            if (pass)
            {
                onEdge = false;
                prevPointerWindow = pointerWindow; // Remember the window pointer is in
                brokeFromTime = current.moveTime;
                brokeFromMonitor = currentMonitor;
                currentMonitor = getCurrentMonitor(x, y); // find the new monitor
            }
            else
            {
                if (x <= currentMonitor->x + cfgResistanceMargins && dx <= 0)
                    x = currentMonitor->x + cfgResistanceMargins;
                if (y <= currentMonitor->y + cfgResistanceMargins && dy <= 0)
                    y = currentMonitor->y + cfgResistanceMargins;
                if (x >= currentMonitor->x + currentMonitor->w - cfgResistanceMargins && dx >= 0)
                    x = currentMonitor->x + currentMonitor->w - cfgResistanceMargins;
                if (y >= currentMonitor->y + currentMonitor->h && dy >= 0)
                    y = currentMonitor->y + currentMonitor->h - cfgResistanceMargins;

                /*
                Manually setting the position causes the pointer to 'flicker' because of the delay
                between the movePointer() call and actual pointer update on screen.
                We confine the pointer in the previous window for cases when there is a fullscreen window.
                */
                movePointer(x, y, prevPointerWindow);
            }
            
        }
    }
    else
    {
        currentMonitor = getCurrentMonitor(x, y);
    }
    
}

int main(int argc, char **argv)
{
    // ---Load config---
    loadConfig();

    // ---Get display---
    if( (display = XOpenDisplay(NULL)) == NULL )
    {
        std::cerr << "Cannot open Display! Exiting..." << std::endl;
        return -1;
    }

    rootWindow = XDefaultRootWindow(display);
    XAllowEvents(display, AsyncBoth, CurrentTime);

    // ---Load the extension---
    int xiExtOpcode;

    int ev, err;
    if (!XQueryExtension(display, "XInputExtension", &xiExtOpcode, &ev, &err))
    {
        std::cerr << "XInput extension is not available. Required to run sticky-cursor-screen-edges." << std::endl;
        return -1;
    }

    // ---Check the version---
    int major_op = 2;
    int minor_op = 2;
    int result = XIQueryVersion(display, &major_op, &minor_op);
    if (result == BadRequest)
    {
        std::cerr << "Required version of XInput is not supported." << std::endl;
        return -1;
    }
    else if (result != Success)
    {
        std::cerr << "Couldn't check version of XInput" << std::endl;
        return -1;
    }

    // ---Select XI events---
    XIEventMask masks[1];
    unsigned char mask[(XI_LASTEVENT + 7)/8];

    memset(mask, 0, sizeof(mask));
    XISetMask(mask, XI_RawMotion);

    masks[0].deviceid = XIAllMasterDevices;
    masks[0].mask_len = sizeof(mask);
    masks[0].mask = mask;

    XISelectEvents(display, DefaultRootWindow(display), masks, 1);
    XFlush(display);

    // ---Monitor list---
    updateMonitorList();
    // notify of resolution changes
    XSelectInput(display, rootWindow, StructureNotifyMask);

    // ---Handle all events---
    XEvent xevent;
    bool captureMoveEvent;
    while(1)
    {
        XNextEvent(display, &xevent);

        if (XGetEventData(display, &xevent.xcookie))
        {
            XGenericEventCookie *cookie = &xevent.xcookie;
            
            if (cookie->extension == xiExtOpcode && cookie->evtype == XI_RawMotion)
            {
                // This is the event we were looking for
                XIDeviceEvent *motionEvent = (XIDeviceEvent*)cookie->data;

                //printf("Move: %6.1f, %6.1f\n", motionEvent->event_x, motionEvent->event_y);

                Window childWnd, parentDummy;
                int root_x, root_y, win_x, win_y;
                unsigned int maskDummy;
                XQueryPointer(display, rootWindow, &rootWindow, &childWnd,
                    &root_x, &root_y, &win_x, &win_y, &maskDummy);

                //captureMoveEvent = true;
                // handle movement
                pointerMoved(root_x, root_y, motionEvent->event_x, motionEvent->event_y, childWnd);
            }
            XFreeEventData(display, cookie);
        }
        else switch (xevent.type)
        {
            case ConfigureNotify:
                updateMonitorList();
                break;
            case MotionNotify:
                //if (captureMoveEvent)
                //{
                    // handle movement
                    //pointerMoved(xevent.xmotion.x_root, xevent.xmotion.y_root);
                    //std::cout << xevent.xmotion.x_root << "," << xevent.xmotion.y << std::endl;

                    // Stop tracking events to prevent infinite loop
                    /*if (xevent.xmotion.window)
                    {
                        XSelectInput(display, xevent.xmotion.window, 0);
                        captureMoveEvent = false;
                    }*/
                    captureMoveEvent = false;
                //}
                break;
        }
    }

    return 0;
}

