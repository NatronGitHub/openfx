
#ifndef _ofxDialog_h_
#define _ofxDialog_h_

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include "ofxCore.h"
#include "ofxProperty.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @file ofxDialog.h

This file contains an optional suite which should be used to request an action to 
be called on the host UI thread. This may be used to popup a native OS dialog
from a host parameter changed action.

When a host uses a fullscreen window and is running the OFX plugins in another thread
it can lead to a lot of conflicts if that plugin will try to open its own window.

This suite provides the functionality for a plugin to request running its dialog
in the UI thread, or any other action that can only be executed in the UI thread,
and informing the host it will do this so it can take the appropriate
actions needed. (Like lowering its priority etc..)
*/

/** @brief The name of the Dialog suite, used to fetch from a host via
    OfxHost::fetchSuite
 */
#define kOfxDialogSuite		"OfxDialogSuite"

/** @brief Action called after an instance has requested a 'Dialog'

The arguments to the action are...
  \arg handle - handle to the plug-in instance, cast to an
                \ref OfxImageEffectHandle
  \arg inArgs - has the following properties
              - kOfxPropInstanceData    - Pointer which was provided when the plugin requested the Dialog
  \arg outArgs - is redundant and set to null

   When the plugin receives this action it is thus safe to popup a dialog, or any other
   taks that should be executed by the plugin in the UI thread.
   It runs in the host's UI thread, which may differ from the main OFX processing thread.
   Plugin should return from this action when all Dialog interactions are done.
   At that point the host will continue again.
   The host will not send any other messages asynchronous to this one.

A plugin can return...
  - ::kOfxStatOK, the action was trapped and all was well
  - ::kOfxStatFailed, something went wrong, but no error code appropriate,
    the plugin should to post a message if possible.

    @actiondef
    inArgs:
    outArgs:
*/
#define  kOfxActionDialog	"OfxActionDialog"

 
/** @brief OFX suite that provides provides the ability to execute an action in the UI thread.

    @deprecated - deprecated in v1.5 in favor of OfxDialogSuiteV2
 */
typedef struct OfxDialogSuiteV1
{
  /** @brief Request the host to send a kOfxActionDialog to the plugin from its UI thread.
  \pre
    - user_data: A pointer to any user data
  \post
  @returns
    - ::kOfxStatOK - The host has queued the request and will send an 'OfxActionDialog'
    - ::kOfxStatFailed - The host has no provision for this or can not deal with it currently.
  */
  OfxStatus (*requestDialog)( void *instanceData );
  
  /** @brief Inform the host of redraw event so it can redraw itself
      If the host runs fullscreen in OpenGL, it would otherwise not receive
redraw event when a dialog in front would catch all events.
  \pre
  \post
  @returns
    - ::kOfxStatReplyDefault
  */
  OfxStatus (*notifyRedrawPending)( void );
} OfxDialogSuiteV1;

/** @brief OFX suite that provides provides the ability to execute an action in the UI thread.
 */
typedef struct OfxDialogSuiteV2
{
  /** @brief Request the host to send a kOfxActionDialog action to the plugin from its UI thread.
  \pre
    - instance: Handle to the instance that requests the dialog, and will receive the kOfxActionDialog.
    - inArgs: The inArgs of the action that triggered the request.
    - instanceData: A pointer to any instance data, which will be passed back in kOfxActionDialog.
        May be used to hold dialog parameters.
  \post
  @returns
    - ::kOfxStatOK - The host has queued the request and will send an 'OfxActionDialog'
    - ::kOfxStatFailed - The host has no provision for this or can not deal with it currently.
  */
  OfxStatus (*requestDialog)( OfxImageEffectHandle instance, OfxPropertySetHandle inArgs, void *instanceData );
  
  /** @brief Inform the host of redraw event so it can redraw itself
      If the host runs fullscreen in OpenGL, it would otherwise not receive
redraw event when a dialog in front would catch all events.
  \pre
    - instance: Handle to the instance that informs the host.
    - inArgs: The inArgs of the action that triggered the notification.
  \post
  @returns
    - ::kOfxStatReplyDefault
  */
  OfxStatus (*notifyRedrawPending)( OfxImageEffectHandle instance, OfxPropertySetHandle inArgs );
} OfxDialogSuiteV2;


#ifdef __cplusplus
}
#endif


#endif

