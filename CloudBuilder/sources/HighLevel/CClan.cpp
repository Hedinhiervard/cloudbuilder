//
//  CClan.cpp
//  CloudBuilder
//
//  Created by Roland Van Leeuwen on 10/04/12.
//  Copyright (c) 2012 Clan of the Cloud. All rights reserved.
//

#include <stdlib.h>
#include <stdio.h>

#include "CClan.h"
#include "CGameManager.h"
#include "CTribeManager.h"
#include "CloudBuilder_private.h"
#include "CUserManager.h"
#include "CFilesystem.h"
#include "CClannishRESTProxy.h"
#include "CMatchManager.h"
#include "CStoreManager.h"
#include "curltool.h"
#include "cotc_thread.h"

using namespace CotCHelpers;

#if defined(__IOS__) || (defined(__MACOS__) && (PRODUCT_NAME != CloudBuilderMacOSX64Console))
#include "GameCenterHandler.h"

#endif

#ifndef __IOS__
void platformSuspend()
{
}
void platformResume()
{
}
#endif


static bool startsWith (const char* base, const char* str) {
	return (strstr(base, str) - base) == 0;
}

static bool endsWith (const char* base, const char* str) {
	size_t blen = strlen(base);
	size_t slen = strlen(str);
	return (blen >= slen) && (0 == strcmp(base + blen - slen, str));
}

LOG_LEVEL g_debugLevel = LOG_LEVEL_WARNING;

namespace CloudBuilder {
#	ifdef DEBUG
		/**
		 * This is a very simple thread that checks whether a call to ProcessIdleTasks is
		 * done and issues a warning otherwise. Set the boolean member to true when called.
		 */
		struct CClan_ProcessIdleTasksCheckThread: CotCHelpers::CThread {
			volatile bool hasCalledProcessIdleTasksOnce, shouldStop;
			CClan_ProcessIdleTasksCheckThread() : hasCalledProcessIdleTasksOnce(false), shouldStop(false) {}
			virtual void Run() {
				// Wait for a total of 100x100ms (10 sec)
				for (int i = 0; i < 100 && !shouldStop; i++) {
					usleep(100 * 1000);
				}
				if (!hasCalledProcessIdleTasksOnce && !shouldStop) {
					CONSOLE_ERROR("!!!! You have not called CClan::ProcessIdleTasks, thus no async operation can complete !!!!");
				}
			}
		};
		CClan_ProcessIdleTasksCheckThread *checkThread;
#	endif

	enum ClanStatus {
		setupRunning=1,
		setupCompletedWithErr,
		setupCompleted,
		userLoged
	};

	static singleton_holder<CClan> managerSingleton;
	extern owned_ref<CDelegate<void(CHttpFailureEventArgs&)>> g_failureDelegate;
	
	CClan::CClan() :
		restoreLoginHandler(*(new CGloballyKeptHandler<CResultHandler>))
	{
		CotCHelpers::Init();
        sprintf(g_curlUserAgent, "%s-%s-%s", LIBCOTC_UA, LIBCOTC_OS, LIBCOTC_VERSION);
        
		// Avoid any activity on the first app resume (wait for a setup)
		hasCalledHandleUrl = true;
		isActive = true;

#ifdef DEBUG
		checkThread = new CClan_ProcessIdleTasksCheckThread;
		checkThread->Start();
#endif
	}

	CClan::~CClan() {
		delete &restoreLoginHandler;
		// Terminate other tasks and release memory
#ifdef DEBUG
		// Terminate the check thread
		checkThread->shouldStop = true;
		checkThread->Join();
		checkThread->Release(), checkThread = NULL;
#endif
	}
	
	CClan* CClan::Instance() {
		return managerSingleton.Instance();
	}

	void CClan::Terminate() {
		http_terminate();
		CallbackStack::removeAllPendingCallbacksWithoutCallingThem();
		// Important: all these managers shouldn't do anything sensible in their terminate method,
		// since the managers are destroyed in a random order. They shouldn't rely on any other
		// manager, nor execute any HTTP request or so.
		CMatchManager::Instance()->Terminate();
		CClannishRESTProxy::Instance()->Terminate();
		CGameManager::Instance()->Terminate();
		CTribeManager::Instance()->Terminate();
		CUserManager::Instance()->Terminate();
		CFilesystemManager::Instance()->Terminate();
		CStoreManager::Instance()->Terminate();
		managerSingleton.Release();
	}
	
	bool CClan::isUserLogged() {
		return CClannishRESTProxy::Instance()->isLoggedIn();
	}
	
	bool CClan::isSetup() {
		return CClannishRESTProxy::Instance()->isSetup();
	}

	void CClan::Setup(const CHJSON* aConfiguration, CResultHandler *handler) {
		if (aConfiguration->GetBool("facebook") || aConfiguration->GetBool("googleplus")) {
			CONSOLE_ERROR("Options facebook or googleplus are obsolete !");
		}

		hasCalledHandleUrl = false;

		// Set the app folder
		const char *appFolder = aConfiguration->GetString("appFolder");
		if (!appFolder) { appFolder = "CotC/untitledApp"; }
		CFilesystemManager::Instance()->SetFolderHint(appFolder);

		// After the CClannishDBProxy::Setup, call CUserManager::DoSetupProcesses
		struct CallUserManagerSetup: CInternalResultHandler {
			_BLOCK1(CallUserManagerSetup, CInternalResultHandler, CResultHandler*, handler);
			void Done(const CCloudResult *result) {
					CUserManager::Instance()->DoSetupProcesses(handler);
			}
		};
		
		// Pass the request
		mAutoresume = aConfiguration->GetBool("autoresume");
		
		owned_ref<CHJSON> json (aConfiguration->Duplicate());
		json->Put("sdkVersion", SDKVERSION);
		if (mAutoresume)
			CClannishRESTProxy::Instance()->Setup(json, new CallUserManagerSetup(handler));
		else
			CClannishRESTProxy::Instance()->Setup(json, MakeBridgeDelegate(handler));
	}

	void CClan::SetLogLevel(LOG_LEVEL logLevel) {
		g_debugLevel = logLevel;
	}

	void CClan::Ping(CResultHandler *handler) {
		CClannishRESTProxy::Instance()->Ping(MakeBridgeDelegate(handler));
	}

	void CClan::Suspend() {
		// App suspended
		CONSOLE_VERBOSE("Suspended\n");
		isActive = false;
		CClannishRESTProxy::Instance()->Suspend();
	}
	
	void CClan::Resume() {
		CONSOLE_VERBOSE("Resumed\n");
		isActive = true;
		// If the app is resumed not as a result of HandleURL it means that any facebook/google+ operation has to be canceled on iOS
		if (!hasCalledHandleUrl) {
			// <exterr>->CancelAnyPendingRequest();
		}
		hasCalledHandleUrl = false;
		CClannishRESTProxy::Instance()->Resume();
	}

	void CClan::HandleURL(const CotCHelpers::CHJSON *aOptions, CResultHandler *handler) {
		hasCalledHandleUrl = true;
		CONSOLE_VERBOSE("Just received URL\n");
		const char *url = aOptions->GetString("url");
		if ( strstr(url, "://cotc/") != 0) {
			if (strstr(url, "://cotc/token=") != 0) {
				if (!isUserLogged()) {
					const char *token = strchr(url, '=') + 1;
					CHJSON j;
					j.Put("id", "n/a");
					j.Put("secret", token);
					j.Put("network", "restore");
					CUserManager::Instance()->LoginNetwork(&j, handler);
				} else
					return InvokeHandler(handler, enAlreadyLoggedWhileRestoringASession);
			}
			else
				return InvokeHandler(handler, enNoErr);
		} else
			return InvokeHandler(handler, enNoErr);
	}
	
	void CClan::ProcessIdleTasks() {
#ifdef DEBUG
		checkThread->hasCalledProcessIdleTasksOnce = true;
#endif
		while (CallbackStack::popCallback()) {
			// Unstack all pending callbacks at once
		}
	}

	void CClan::SetHttpFailureCallback(CDelegate<void(CHttpFailureEventArgs&)> *aCallback) {
		g_failureDelegate <<= aCallback;
	}

}
