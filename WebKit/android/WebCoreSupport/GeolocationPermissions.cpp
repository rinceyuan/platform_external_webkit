/*
 * Copyright 2009, The Android Open Source Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "GeolocationPermissions.h"

#include "Frame.h"
#include "Geolocation.h"
#include "Navigator.h"
#include "WebViewCore.h"

using WebCore::Frame;
using WebCore::String;
using WebCore::Timer;

namespace android {

// TODO(steveblock): Write the permanent permissions to stable storage when
// the browser closes and read them on startup.
GeolocationPermissions::PermissionsMap GeolocationPermissions::s_permanentPermissions;
GeolocationPermissions::GeolocationPermissionsVector GeolocationPermissions::s_instances;

GeolocationPermissions::GeolocationPermissions(WebViewCore* webViewCore, Frame* mainFrame)
    : m_webViewCore(webViewCore)
    , m_mainFrame(mainFrame)
    , m_timer(this, &GeolocationPermissions::timerFired)

{
    s_instances.append(this);
    ASSERT(m_webViewCore);
}

GeolocationPermissions::~GeolocationPermissions()
{
    size_t index = s_instances.find(this);
    s_instances.remove(index);
}

void GeolocationPermissions::queryPermissionState(Frame* frame)
{
    // We use SecurityOrigin::toString to key the map. Note that testing
    // the SecurityOrigin pointer for equality is insufficient.
    String originString = frame->document()->securityOrigin()->toString();

    // See if we have a record for this origin in the temporary permissions for
    // this tab. These take precedence over permanent permissions.
    PermissionsMap::const_iterator iter = m_temporaryPermissions.find(originString);
    PermissionsMap::const_iterator end = m_temporaryPermissions.end();
    if (iter != end) {
        bool allow = iter->second;
        makeAsynchronousCallbackToGeolocation(originString, allow);
        return;
    }

    // Check the permanent permisions.
    iter = s_permanentPermissions.find(originString);
    end = s_permanentPermissions.end();
    if (iter != end) {
        bool allow = iter->second;
        makeAsynchronousCallbackToGeolocation(originString, allow);
        return;
    }

    // If there's no pending request, prompt the user.
    if (m_originInProgress.isEmpty()) {
        m_originInProgress = originString;

        // Although multiple tabs may request permissions for the same origin
        // simultaneously, the routing in WebViewCore/CallbackProxy ensures that
        // the result of the request will make it back to this object, so
        // there's no need for a globally unique ID for the request.
        m_webViewCore->geolocationPermissionsShowPrompt(m_originInProgress);
        return;
    }

    // If the request in progress is not for this origin, queue this request.
    if ((m_originInProgress != originString)
        && (m_queuedOrigins.find(originString) != WTF::notFound))
        m_queuedOrigins.append(originString);
}

void GeolocationPermissions::makeAsynchronousCallbackToGeolocation(String origin, bool allow)
{
    m_callbackData.origin = origin;
    m_callbackData.allow = allow;
    m_timer.startOneShot(0);
}

void GeolocationPermissions::providePermissionState(String origin, bool allow, bool remember)
{
    // It's possible that this method is called with an origin that doesn't
    // match m_originInProgress. This can occur if this object is reset
    // while a permission result is in the process of being marshalled back to
    // the WebCore thread from the browser. In this case, we simply ignore the
    // call.
    if (origin != m_originInProgress)
        return;

    maybeCallbackFrames(m_originInProgress, allow);
    recordPermissionState(origin, allow, remember);

    // If the permissions are set to be remembered, cancel any queued requests
    // for this domain in other tabs.
    if (remember)
        cancelPendingRequestsInOtherTabs(m_originInProgress);

    // If there are other requests queued, start the next one.
    if (!m_queuedOrigins.isEmpty()) {
        m_originInProgress = m_queuedOrigins.first();
        m_queuedOrigins.remove(0);
        m_webViewCore->geolocationPermissionsShowPrompt(m_originInProgress);
    }

    m_originInProgress = "";
}

void GeolocationPermissions::recordPermissionState(String origin, bool allow, bool remember)
{
    if (remember) {
        s_permanentPermissions.set(m_originInProgress, allow);
        // If we already have a temporary permission for this origin, remove it,
        // so that later clearing the permanent permission works as expected.
        m_temporaryPermissions.remove(origin);
    } else {
        // It's possible that another tab recorded a permanent permission for
        // this origin while our request was in progress, but we record it
        // anyway.
        m_temporaryPermissions.set(m_originInProgress, allow);
    }
}

void GeolocationPermissions::cancelPendingRequestsInOtherTabs(String origin)
{
    for (GeolocationPermissionsVector::const_iterator iter = s_instances.begin();
         iter != s_instances.end();
         ++iter)
        (*iter)->cancelPendingRequests(origin);
}

void GeolocationPermissions::cancelPendingRequests(String origin)
{
    size_t index = m_queuedOrigins.find(origin);
    if (index != WTF::notFound) {
        // Get the permission from the permanent list.
        PermissionsMap::const_iterator iter = s_permanentPermissions.find(origin);
        ASSERT(iter != (PermissionsMap::const_iterator end = s_permanentPermissions.end()));
        bool allow = iter->second;

        maybeCallbackFrames(origin, allow);

        m_queuedOrigins.remove(index);
    }
}

void GeolocationPermissions::timerFired(Timer<GeolocationPermissions>* timer)
{
    ASSERT(timer == m_timer);
    maybeCallbackFrames(m_callbackData.origin, m_callbackData.allow);
}

void GeolocationPermissions::resetTemporaryPermissionStates()
{
    m_originInProgress = "";
    m_queuedOrigins.clear();
    m_temporaryPermissions.clear();
    // If any permission results are being marshalled back to this thread, this
    // will render them inefective.
    m_timer.stop();

    m_webViewCore->geolocationPermissionsHidePrompt();
}

void GeolocationPermissions::maybeCallbackFrames(String origin, bool allow)
{
    // We can't track which frame issued the request, as frames can be deleted
    // or have their contents replaced. Even uniqueChildName is not unique when
    // frames are dynamically deleted and created. Instead, we simply call back
    // to the Geolocation object in all frames from the correct origin.
    for (Frame* frame = m_mainFrame; frame; frame = frame->tree()->traverseNext()) {
        if (origin == frame->document()->securityOrigin()->toString()) {
            // If the page has changed, it may no longer have a Geolocation
            // object.
            Geolocation* geolocation = frame->domWindow()->navigator()->optionalGeolocation();
            if (geolocation)
                geolocation->setIsAllowed(allow);
        }
    }
}

GeolocationPermissions::OriginSet GeolocationPermissions::getOrigins()
{
    OriginSet origins;
    PermissionsMap::const_iterator end = s_permanentPermissions.end();
    for (PermissionsMap::const_iterator iter = s_permanentPermissions.begin(); iter != end; ++iter)
        origins.add(iter->first);
    return origins;
}

bool GeolocationPermissions::getAllowed(String origin)
{
    bool allowed = false;
    PermissionsMap::const_iterator iter = s_permanentPermissions.find(origin);
    PermissionsMap::const_iterator end = s_permanentPermissions.end();
    if (iter != end)
        allowed = iter->second;
    return allowed;
}

void GeolocationPermissions::clear(String origin)
{
    PermissionsMap::iterator iter = s_permanentPermissions.find(origin);
    if (iter != s_permanentPermissions.end())
        s_permanentPermissions.remove(iter);
}

void GeolocationPermissions::clearAll()
{
    s_permanentPermissions.clear();
}

}  // namespace android
