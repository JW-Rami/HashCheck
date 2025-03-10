/**
 * HashCheck Shell Extension
 * Original work copyright (C) Kai Liu.  All rights reserved.
 * Modified work copyright (C) 2014, 2016 Christopher Gurnee.  All rights reserved.
 * Modified work copyright (C) 2016 Tim Schlueter.  All rights reserved.
 * Modified work copyright (C) 2021 Mounir IDRASSI.  All rights reserved.
 *
 * Please refer to readme.txt for information about this source code.
 * Please refer to license.txt for details about distribution and modification.
 **/

#include "globals.h"
#include "HashCheckCommon.h"
#include "HashCalc.h"
#include "libs/WinHash.h"
#include <Strsafe.h>
#include <assert.h>

// Control structures, from HashCalc.h
#define  HASHPROPSCRATCH  HASHCALCSCRATCH
#define PHASHPROPSCRATCH PHASHCALCSCRATCH
#define  HASHPROPCONTEXT  HASHCALCCONTEXT
#define PHASHPROPCONTEXT PHASHCALCCONTEXT
#define  HASHPROPITEM     HASHCALCITEM
#define PHASHPROPITEM    PHASHCALCITEM


/*============================================================================*\
	Function declarations
\*============================================================================*/

// Worker thread
VOID __fastcall HashPropWorkerMain( PHASHPROPCONTEXT phpctx );
VOID WINAPI HashPropRestart( PHASHPROPCONTEXT phpctx );

// Dialog general
VOID WINAPI HashPropDlgInit( PHASHPROPCONTEXT phpctx );
VOID WINAPI HashPropFitDialog( HWND hWnd );
VOID WINAPI HashPropForceLTR( HWND hWndEdit );

// Dialog status
LRESULT CALLBACK HashPropEditProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
LRESULT CALLBACK HashPropResultsProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
VOID WINAPI HashPropUpdateResults( PHASHPROPCONTEXT phpctx, PHASHPROPITEM pItem );
VOID WINAPI HashPropFinalStatus( PHASHPROPCONTEXT phpctx );

// Dialog commands
VOID WINAPI HashPropFindText( PHASHPROPCONTEXT phpctx, BOOL bIncremental );
VOID WINAPI HashPropSaveResults( PHASHPROPCONTEXT phpctx );
VOID WINAPI HashPropDoSaveResults( PHASHPROPCONTEXT phpctx );
VOID WINAPI HashPropSaveResultsCleanup( PHASHPROPCONTEXT phpctx );
VOID WINAPI HashPropOptions( PHASHPROPCONTEXT phpctx );



/*============================================================================*\
	Entry points / main functions
\*============================================================================*/

UINT CALLBACK HashPropCallback( HWND hWnd, UINT uMsg, LPPROPSHEETPAGE ppsp )
{
	HSIMPLELIST hList = (HSIMPLELIST)(ppsp->lParam);

	switch (uMsg)
	{
		case PSPCB_ADDREF:
			SLAddRef(hList);
			break;

		case PSPCB_RELEASE:
			SLRelease(hList);
			break;

		case PSPCB_CREATE:
		{
			PHASHPROPCONTEXT phpctx = SLSetContextSize(hList, sizeof(HASHPROPCONTEXT));

			if (phpctx)
			{
				phpctx->status = INACTIVE;
				phpctx->hListRaw = hList;
				return(1);
			}
		}
	}

	return(0);
}



/*============================================================================*\
	Worker thread
\*============================================================================*/

VOID __fastcall HashPropWorkerMain( PHASHPROPCONTEXT phpctx )
{
	// Note that ALL message communication to and from the main window MUST
	// be asynchronous, or else there may be a deadlock.

	PHASHPROPITEM pItem;
    WHCTXEX whctx;

	// Prep: if not already done, expand directories, establish prefix, etc.
    if (! (phpctx->dwFlags & HPF_HLIST_PREPPED))
    {
        PostMessage(phpctx->hWnd, HM_WORKERTHREAD_TOGGLEPREP, (WPARAM)phpctx, TRUE);
        if (! HashCalcPrepare(phpctx))
            return;
        phpctx->dwFlags |= HPF_HLIST_PREPPED;
    }
	PostMessage(phpctx->hWnd, HM_WORKERTHREAD_TOGGLEPREP, (WPARAM)phpctx, FALSE);

	// Which checksum types we want to calculate
    // (this is loaded earlier in HashPropDlgInit())
    DWORD checksumFlags = (UINT8)phpctx->opt.dwChecksums;

    // Read buffer
    PBYTE pbBuffer = (PBYTE)VirtualAlloc(NULL, READ_BUFFER_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (pbBuffer == NULL)
        return;

#ifdef _TIMED
    DWORD dwStarted;
    dwStarted = GetTickCount();
#endif

	while (pItem = SLGetDataAndStep(phpctx->hList))
	{
        // Some results might already be present if the user changes which checksum types
        // to calculate and we're going through the list a second+ time for all/some items;
        // only calculate the checksums we don't already have (usually all those requested)
        whctx.dwFlags = checksumFlags & ~pItem->results.dwFlags;

		// Get the hash
		WorkerThreadHashFile(
			(PCOMMONCONTEXT)phpctx,
			pItem->szPath,
			&whctx,
			&pItem->results,
			pbBuffer,
			NULL, 0, NULL, NULL
#ifdef _TIMED
          , &pItem->dwElapsed
#endif
        );

        if (phpctx->status == PAUSED)
            WaitForSingleObject(phpctx->hUnpauseEvent, INFINITE);
		if (phpctx->status == CANCEL_REQUESTED)
			break;

		// Update the UI
		++phpctx->cSentMsgs;
		PostMessage(phpctx->hWnd, HM_WORKERTHREAD_UPDATE, (WPARAM)phpctx, (LPARAM)pItem);
	}
#ifdef _TIMED
    phpctx->dwElapsed = GetTickCount() - dwStarted;
#endif
    VirtualFree(pbBuffer, 0, MEM_RELEASE);
}



/*============================================================================*\
	Dialog general
\*============================================================================*/

INT_PTR CALLBACK HashPropDlgProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	PHASHPROPCONTEXT phpctx;

	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			phpctx = (PHASHPROPCONTEXT)SLGetContextData((HSIMPLELIST)(((LPPROPSHEETPAGE)lParam)->lParam));

			// Associate the window with the context and vice-versa
			phpctx->hWnd = hWnd;
			SetWindowLongPtr(hWnd, DWLP_USER, (LONG_PTR)phpctx);

			HashPropDlgInit(phpctx);

			phpctx->pfnWorkerMain = HashPropWorkerMain;
			phpctx->hThread = CreateThreadCRT(NULL, phpctx);

			if (!phpctx->hThread)
				WorkerThreadCleanup((PCOMMONCONTEXT)phpctx);

			return(TRUE);
		}

		case WM_SHOWWINDOW:
		{
			phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);

			if (wParam && !(phpctx->dwFlags & HPF_HAS_RESIZED))
			{
				// We wait for WM_SHOWWINDOW because the reported size may not
				// be accurate during WM_INITDIALOG
				HashPropFitDialog(hWnd);
				phpctx->dwFlags |= HPF_HAS_RESIZED;
			}

			HashPropForceLTR(GetDlgItem(hWnd, IDC_RESULTS));

			break;
		}

		case WM_ENDSESSION:
        {
            if (wParam == FALSE)  // if TRUE, fall through to WM_DESTROY
                break;
        }
		case WM_DESTROY:
		{
			phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);

			// Undo the search box subclassing
			SetWindowLongPtr(
				GetDlgItem(hWnd, IDC_SEARCHBOX),
				GWLP_WNDPROC,
				(LONG_PTR)phpctx->wpSearchBox
			);

            // Undo the results box subclassing
            SetWindowLongPtr(
                GetDlgItem(hWnd, IDC_RESULTS),
                GWLP_WNDPROC,
                (LONG_PTR)phpctx->wpResultsBox
            );

			// Kill the worker thread; HCF_EXIT_PENDING indicates
            // we don't need to do any further GUI updates
			phpctx->dwFlags |= HCF_EXIT_PENDING;
			WorkerThreadStop((PCOMMONCONTEXT)phpctx);
			WorkerThreadCleanup((PCOMMONCONTEXT)phpctx);

			// Cleanup
            HashPropSaveResultsCleanup(phpctx);
			if (phpctx->hFont) DeleteObject(phpctx->hFont);
			if (phpctx->hList) SLRelease(phpctx->hList);

			break;
		}

		case WM_COMMAND:
		{
			phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);

			switch (LOWORD(wParam))
			{
				case IDC_SEARCHBOX:
				{
					if (HIWORD(wParam) == EN_CHANGE)
					{
						HashPropFindText(phpctx, TRUE);
						return(TRUE);
					}

					break;
				}

				case IDC_FIND_NEXT:
				{
					HashPropFindText(phpctx, FALSE);
					return(TRUE);
				}

				case IDC_PAUSE:
				{
					WorkerThreadTogglePause((PCOMMONCONTEXT)phpctx);
					return(TRUE);
				}

				case IDC_STOP:
				{
                    phpctx->dwFlags |= HPF_INTERRUPTED;
					WorkerThreadStop((PCOMMONCONTEXT)phpctx);
                    HashPropSaveResultsCleanup(phpctx);
					return(TRUE);
				}

				case IDC_SAVE:
				{
					HashPropSaveResults(phpctx);
					return(TRUE);
				}

				case IDC_OPTIONS:
				{
					HashPropOptions(phpctx);
					return(TRUE);
				}

				case IDC_RESULTS:
				{
					if (HIWORD(wParam) == EN_ALIGN_RTL_EC)
					{
						// Do not allow the textbox to go into RTL; unfortunately,
						// we get this notification only if the order was changed
						// by a keyboard shortcut, not if it was changed by the
						// context menu--in fact, no notification of any sort
						// is sent when it is changed by the context menu, which
						// seems like it may be a bug in Windows.
						HashPropForceLTR((HWND)lParam);
						return(TRUE);
					}

					break;
				}
			}

			break;
		}

		case WM_TIMER:
		{
			// Vista: Workaround to fix their buggy progress bar
			KillTimer(hWnd, TIMER_ID_PAUSE);
			phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			if (phpctx->status == PAUSED)
				SetProgressBarPause((PCOMMONCONTEXT)phpctx, PBST_PAUSED);
			return(TRUE);
		}

		case HM_WORKERTHREAD_DONE:
		{
			phpctx = (PHASHPROPCONTEXT)wParam;
			WorkerThreadCleanup((PCOMMONCONTEXT)phpctx);
            if (phpctx->hFileOut != INVALID_HANDLE_VALUE)
                HashPropDoSaveResults(phpctx);
			HashPropFinalStatus(phpctx);
			return(TRUE);
		}

		case HM_WORKERTHREAD_UPDATE:
		{
			phpctx = (PHASHPROPCONTEXT)wParam;
			++phpctx->cHandledMsgs;
            // If we're restarting the worker thread, no need to update the GUI
            if (phpctx->dwFlags & HCF_RESTARTING)
            {
                // Only restart the worker thread once we're caught up on handled messages
                if (phpctx->cHandledMsgs >= phpctx->cSentMsgs)
                    HashPropRestart(phpctx);
            }
            else
                HashPropUpdateResults(phpctx, (PHASHPROPITEM)lParam);
			return(TRUE);
		}

		case HM_WORKERTHREAD_TOGGLEPREP:
		{
			HashCalcTogglePrep((PHASHPROPCONTEXT)wParam, (BOOL)lParam);
			return(TRUE);
		}
	}

	return(FALSE);
}

LRESULT CALLBACK HashPropEditProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	PHASHPROPCONTEXT phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(
		(HWND)GetWindowLongPtr(hWnd, GWLP_HWNDPARENT),
		DWLP_USER
	);

	if (wParam == VK_RETURN)
	{
		if (uMsg == WM_GETDLGCODE)
		{
			return(DLGC_WANTALLKEYS);
		}
		else if (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST)
		{
			if (uMsg == WM_KEYDOWN)
				HashPropFindText(phpctx, FALSE);

			return(0);
		}
	}

	return(CallWindowProc(phpctx->wpSearchBox, hWnd, uMsg, wParam, lParam));
}

LRESULT CALLBACK HashPropResultsProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PHASHPROPCONTEXT phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(
        (HWND)GetWindowLongPtr(hWnd, GWLP_HWNDPARENT),
        DWLP_USER
    );

    if (wParam == VK_ESCAPE)
    {
        if (uMsg == WM_GETDLGCODE)
        {
            return(DLGC_WANTALLKEYS);
        }
        else if (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST)
        {
            PostMessage(phpctx->hWnd, uMsg, wParam, lParam);

            return(0);
        }
    }
    else if (uMsg == WM_CHAR && wParam == 1)  // CTRL-a
    {
        SendMessage(hWnd, EM_SETSEL, 0, -1);

        return(1);
    }

    return(CallWindowProc(phpctx->wpResultsBox, hWnd, uMsg, wParam, lParam));
}

VOID WINAPI HashPropDlgInit( PHASHPROPCONTEXT phpctx )
{
	HWND hWnd = phpctx->hWnd;
	UINT i;

	// Load strings
	{
		static const UINT16 arStrMap[][2] =
		{
			{ IDC_STATUSBOX, IDS_HP_STATUSBOX },
			{ IDC_FIND_NEXT, IDS_HP_FIND      },
			{ IDC_PAUSE,     IDS_HP_PAUSE     },
			{ IDC_STOP,      IDS_HP_STOP      },
			{ IDC_SAVE,      IDS_HP_SAVE      },
			{ IDC_OPTIONS,   IDS_HP_OPTIONS   }
		};

		for (i = 0; i < countof(arStrMap); ++i)
			SetControlText(hWnd, arStrMap[i][0], arStrMap[i][1]);
	}

    // Load the two configuration items we need
    phpctx->opt.dwFlags = HCOF_FONT | HCOF_CHECKSUMS;
    OptionsLoad(&phpctx->opt);

	// Initialize the results text box
	{
		// Set the font
		if (phpctx->hFont = CreateFontIndirect(&phpctx->opt.lfFont))
			SendDlgItemMessage(hWnd, IDC_RESULTS, WM_SETFONT, (WPARAM)phpctx->hFont, FALSE);

		// Eliminate the text limit
		SendDlgItemMessage(hWnd, IDC_RESULTS, EM_SETLIMITTEXT, 0, 0);

        // Subclass it to handle CTRL-a
        phpctx->wpResultsBox = (WNDPROC)SetWindowLongPtr(
            GetDlgItem(hWnd, IDC_RESULTS),
            GWLP_WNDPROC,
            (LONG_PTR)HashPropResultsProc
        );
	}

	// Initialize the search text box
	{
		// Subclass it to handle pressing the return key
		phpctx->wpSearchBox = (WNDPROC)SetWindowLongPtr(
			GetDlgItem(hWnd, IDC_SEARCHBOX),
			GWLP_WNDPROC,
			(LONG_PTR)HashPropEditProc
		);
	}

	// Initialize miscellaneous stuff
	{
		phpctx->hList = SLCreateEx(TRUE);
		phpctx->dwFlags = 0;
		phpctx->cTotal = 0;
		phpctx->cSuccess = 0;
		phpctx->obScratch = 0;
        phpctx->hThread = NULL;
        phpctx->hUnpauseEvent = NULL;
        phpctx->hFileOut = INVALID_HANDLE_VALUE;
		ZeroMemory(&phpctx->ofn, sizeof(phpctx->ofn));
	}
}

VOID WINAPI HashPropFitDialog( HWND hWnd )
{
	HWND hWndResults = GetDlgItem(hWnd, IDC_RESULTS);
	RECT rcDlg, rcTop, rcBottom;
	INT dy, dx, i;

	GetClientRect(hWnd, &rcDlg);
	GetWindowRect(hWndResults, &rcTop);
	GetWindowRect(GetDlgItem(hWnd, IDC_OPTIONS), &rcBottom);

	// This is kosher because a RECT is really just two POINTs...
	ScreenToClient(hWnd, (PPOINT)&rcTop.left);
	ScreenToClient(hWnd, (PPOINT)&rcTop.right);
	ScreenToClient(hWnd, (PPOINT)&rcBottom.right);

	// Get the amount that we need to offset by...
	dy = (rcDlg.bottom - rcBottom.bottom) - (rcTop.top - rcDlg.top);
	dx = (rcDlg.right - rcTop.right) - (rcTop.left - rcDlg.left);

	if (dy > 0)
	{
		// Controls to move by dy
		static const UINT16 arCtrlsMY[] =
		{
			IDC_STATUSBOX,
			IDC_PROG_TOTAL,
			IDC_PROG_FILE,
			IDC_SEARCHBOX,
			IDC_FIND_NEXT,
			IDC_PAUSE,
			IDC_STOP,
			IDC_SAVE,
			IDC_OPTIONS
		};

		// Shift controls down by dy
		for (i = 0; i < countof(arCtrlsMY); ++i)
		{
			HWND hWndCtrl = GetDlgItem(hWnd, arCtrlsMY[i]);
			GetWindowRect(hWndCtrl, &rcBottom);
			ScreenToClient(hWnd, (PPOINT)&rcBottom.left);
			SetWindowPos(hWndCtrl, NULL, rcBottom.left, rcBottom.top + dy,
			             0, 0, SWP_NOSIZE | SWP_NOZORDER);
		}

		// Increase the height of the results box by dy
		SetWindowPos(hWndResults, NULL, 0, 0, rcTop.right - rcTop.left,
		             dy + rcTop.bottom - rcTop.top, SWP_NOMOVE | SWP_NOZORDER);
	}

	if (dx > 0)
	{
		// Controls to resize by dx
		static const UINT16 arCtrlsSX[] =
		{
			IDC_RESULTS,
			IDC_STATUSBOX,
			IDC_PROG_TOTAL,
			IDC_PROG_FILE,
			IDC_SEARCHBOX
		};

		// Controls to move by dx
		static const UINT16 arCtrlsMX[] =
		{
			IDC_FIND_NEXT,
			IDC_PAUSE,
			IDC_STOP,
			IDC_SAVE,
			IDC_OPTIONS
		};

		// Expand controls by dx
		for (i = 0; i < countof(arCtrlsSX); ++i)
		{
			HWND hWndCtrl = GetDlgItem(hWnd, arCtrlsSX[i]);
			GetWindowRect(hWndCtrl, &rcBottom);
			ScreenToClient(hWnd, (PPOINT)&rcBottom.left);
			ScreenToClient(hWnd, (PPOINT)&rcBottom.right);
			SetWindowPos(hWndCtrl, NULL, 0, 0, dx + rcBottom.right - rcBottom.left,
			             rcBottom.bottom - rcBottom.top, SWP_NOMOVE | SWP_NOZORDER);
		}

		// Shift controls right by dx
		for (i = 0; i < countof(arCtrlsMX); ++i)
		{
			HWND hWndCtrl = GetDlgItem(hWnd, arCtrlsMX[i]);
			GetWindowRect(hWndCtrl, &rcBottom);
			ScreenToClient(hWnd, (PPOINT)&rcBottom.left);
			SetWindowPos(hWndCtrl, NULL, rcBottom.left + dx, rcBottom.top,
			             0, 0, SWP_NOSIZE | SWP_NOZORDER);
		}
	}
}

VOID WINAPI HashPropForceLTR( HWND hWndEdit )
{
	DWORD dwExStyle = (DWORD)GetWindowLongPtr(hWndEdit, GWL_EXSTYLE);
	dwExStyle &= ~(WS_EX_RIGHT | WS_EX_RTLREADING | WS_EX_LEFTSCROLLBAR);
	SetWindowLongPtr(hWndEdit, GWL_EXSTYLE, dwExStyle);
}



/*============================================================================*\
	Dialog status
\*============================================================================*/

VOID WINAPI HashPropUpdateResults( PHASHPROPCONTEXT phpctx, PHASHPROPITEM pItem )
{
	HWND hWnd = phpctx->hWnd;
	HWND hWndResults = GetDlgItem(hWnd, IDC_RESULTS);

	/**
	 * It turns out that when hashing large numbers of small files, the
	 * worker thread can far outpace the UI thread, leaving the UI thread
	 * with a large backlog of updates; the solution is to keep track of
	 * the size of this backlog so that steps can be taken to alleviate it.
	 *
	 * 1) cSentMsgs and cHandledMsgs are used to keep track of the backlog;
	 *    cSentMsgs will always be >= to cHandledMsgs (there are no race
	 *    conditions to worry about), and their difference is the number
	 *    of updates pending in the queue AFTER the update currently being
	 *    handled is completed; therefore, zero means no pending backlog.
	 * 2) If there are more than 50 backlogged updates, the worker thread
	 *    will throttle back enough to keep the backlog <= 50.
	 * 3) If there is any backlog at all, results will be coalesced into
	 *    batches to reduce the number of costly EM_REPLACESEL calls.
	 * INVARIANT: the scratch buffer into which the results are coalesced
	 *    has at least enough remaining space for adding the text results
	 *    of a single file (it is cleared before returning if necessary)
	 **/

	PTSTR pszScratchAppend;
    size_t cchMaxBufferRequired = 0;  // max tchar count for text results of one file

    // Check to see of any desired hashes are not present in the results
    if (phpctx->opt.dwChecksums & ~pItem->results.dwFlags)
        // Replace the invalid hashes with X's
        HashCalcClearInvalid(&pItem->results, TEXT('X'));
    else
		// Otherwise, we can increment the success count
		++phpctx->cSuccess;

	// Get the scratch buffer; we will be using the entire scratch struct
	// as a single monolithic buffer
    pszScratchAppend = BYTEADD(&phpctx->scratch, phpctx->obScratch);

	// Copy the file label
	pszScratchAppend += LoadString(g_hModThisDll, IDS_HP_FILELABEL,
		                           pszScratchAppend, MAX_STRINGRES);
    cchMaxBufferRequired += MAX_STRINGRES;

	// Copy the path, appending CRLF
    pszScratchAppend = SSChainNCpy2(
        pszScratchAppend,
        pItem->szPath + phpctx->cchPrefix, pItem->cchPath - phpctx->cchPrefix,
        CRLF, CCH_CRLF
    );
    cchMaxBufferRequired += MAX_PATH_BUFFER - phpctx->cchPrefix + CCH_CRLF;

    // Copy the results
    PTSTR pszScratchBeforeResults = pszScratchAppend;
#define HASH_RESULT_APPEND_op(alg)                                              \
    if (phpctx->opt.dwChecksums & WHEX_CHECK##alg)                              \
        pszScratchAppend = SSChainNCpy3(                                        \
            pszScratchAppend,                                                   \
            HASH_RESULT_op(alg), sizeof(HASH_RESULT_op(alg))/sizeof(TCHAR) - 1, /* the "- 1" excludes the terminating NUL */ \
            pItem->results.szHex##alg, alg##_DIGEST_LENGTH * 2,                 \
            CRLF, CCH_CRLF                                                      \
        );
    FOR_EACH_HASH(HASH_RESULT_APPEND_op)
    cchMaxBufferRequired += pszScratchAppend - pszScratchBeforeResults;  // always the same length

#ifndef _TIMED
    // Append CRLF and a terminating NUL
    pszScratchAppend = SSChainNCpy(
        pszScratchAppend,
        CRLF _T("\0"), CCH_CRLF + 1
    );
    cchMaxBufferRequired += CCH_CRLF + 1;
    pszScratchAppend--;  // it now points to the terminating NUL
#else
    StringCchPrintfEx(pszScratchAppend, 30, &pszScratchAppend, NULL, 0, _T("Elapsed: %d ms") CRLF CRLF, pItem->dwElapsed);
    cchMaxBufferRequired += 30;
#endif

	// Update the new buffer offset for use by the next update
	phpctx->obScratch = (UINT)BYTEDIFF(pszScratchAppend, &phpctx->scratch);

	// Determine if we can skip flushing the buffer
	if ( phpctx->cSentMsgs > phpctx->cHandledMsgs &&
		 phpctx->obScratch + (cchMaxBufferRequired * sizeof(TCHAR)) <= sizeof(HASHPROPSCRATCH) )
	{
		return;
	}

	// Flush the buffer to the text box
	phpctx->obScratch = 0;
	SendMessage(hWndResults, EM_SETSEL, -2, -2);
	SendMessage(hWndResults, EM_REPLACESEL, FALSE, (LPARAM)&phpctx->scratch);

	// ClearType will sometimes leave artifacts, so redraw if the user will
	// be looking at this text for a while
	if (phpctx->cSentMsgs == phpctx->cHandledMsgs)
		InvalidateRect(hWndResults, NULL, FALSE);

	// Yes, this means that if we defer the text box update, we also end up
	// deferring the progress bar update too, which is what we want; progress
	// bar updates are not deferred for unreadable files, but that is an edge
	// case that we should not dwell too much on.
	SendMessage(phpctx->hWndPBTotal, PBM_SETPOS, phpctx->cHandledMsgs, 0);
}

VOID WINAPI HashPropFinalStatus( PHASHPROPCONTEXT phpctx )
{
	TCHAR szBuffer1[MAX_STRINGRES], szBuffer2[MAX_STRINGMSG], szBuffer3[MAX_STRINGMSG];

	// FormatFractionalResults expects an empty format buffer on the first call
	szBuffer1[0] = 0;

	FormatFractionalResults(szBuffer1, szBuffer2, phpctx->cSuccess, phpctx->cTotal);

	LoadString(
		g_hModThisDll,
		IDS_HP_STATUSTEXT_FMT,
		szBuffer1,
		countof(szBuffer1)
	);

	StringCchPrintf(szBuffer3, countof(szBuffer3), szBuffer1, szBuffer2);

#ifndef _TIMED
	SetDlgItemText(phpctx->hWnd, IDC_STATUSBOX, szBuffer3);
#else
    StringCchPrintf(szBuffer2, countof(szBuffer2), _T("%s in %d ms"), szBuffer3, phpctx->dwElapsed);
    SetDlgItemText(phpctx->hWnd, IDC_STATUSBOX, szBuffer2);
#endif

	// Enable search controls
	EnableControl(phpctx->hWnd, IDC_SEARCHBOX, TRUE);
	EnableControl(phpctx->hWnd, IDC_FIND_NEXT, TRUE);

	// Enable the save button only if there exists completed results to save
	if (!(phpctx->dwFlags & HPF_INTERRUPTED) && phpctx->cSuccess > 0)
		EnableControl(phpctx->hWnd, IDC_SAVE, TRUE);
}



/*============================================================================*\
	Dialog commands
\*============================================================================*/

VOID WINAPI HashPropFindText( PHASHPROPCONTEXT phpctx, BOOL bIncremental )
{
	// Since the SimpleList context for hList is unused (the hListRaw ctx
	// is the HP ctx), we might as well make use of that as an easy fussless
	// way to allocate memory without worrying about freeing it

	HWND hWndResults = GetDlgItem(phpctx->hWnd, IDC_RESULTS);
	HWND hWndSearch = GetDlgItem(phpctx->hWnd, IDC_SEARCHBOX);

	SIZE_T cchNeedle = SendMessage(hWndSearch, WM_GETTEXTLENGTH, 0, 0);
	PTSTR pszNeedle = SLSetContextSize(phpctx->hList, ((UINT)cchNeedle + 1) * sizeof(TCHAR));
	PTSTR pszHaystack;
	PTSTR pszFound = NULL;

	DWORD dwPos;

	if (bIncremental)
		SendMessage(hWndResults, EM_GETSEL, (WPARAM)&dwPos, (LPARAM)NULL);
	else
		SendMessage(hWndResults, EM_GETSEL, (WPARAM)NULL, (LPARAM)&dwPos);

	if (pszNeedle && SendMessage(hWndSearch, WM_GETTEXT, cchNeedle + 1, (LPARAM)pszNeedle))
	{
		HLOCAL hResults = (HLOCAL)SendMessage(hWndResults, EM_GETHANDLE, 0, 0);

		// Dangling whitespace should not affect search results
		StrTrim(pszNeedle, TEXT(" \t\r\n"));
		cchNeedle = SSLen(pszNeedle);

		if (cchNeedle && hResults && (pszHaystack = LocalLock(hResults)))
		{
			pszFound = StrStrI(pszHaystack + dwPos, pszNeedle);
			if (!pszFound) pszFound = StrStrI(pszHaystack, pszNeedle);

			if (pszFound)
			{
				dwPos = (DWORD)(pszFound - pszHaystack);
				SendMessage(hWndResults, EM_SETSEL, dwPos, dwPos + cchNeedle);
				SendMessage(hWndResults, EM_SCROLLCARET, 0, 0);
			}

			LocalUnlock(hResults);
		}
	}

	if (cchNeedle == 0 && bIncremental)
	{
		SendMessage(hWndResults, EM_SETSEL, dwPos, dwPos);
	}
	else if (!pszFound)
	{
		TCHAR szBuffer[MAX_STRINGMSG];

		EDITBALLOONTIP ebt;
		ebt.cbStruct = sizeof(ebt);
		ebt.pszTitle = NULL;
		ebt.pszText = szBuffer;
		ebt.ttiIcon = TTI_NONE;

		LoadString(
			g_hModThisDll,
			(cchNeedle) ? IDS_HP_FIND_NOTFOUND : IDS_HP_FIND_NOSTRING,
			szBuffer,
			countof(szBuffer)
		);

		SendMessage(hWndSearch, EM_SHOWBALLOONTIP, 0, (LPARAM)&ebt);
	}
}


VOID WINAPI HashPropSaveResults( PHASHPROPCONTEXT phpctx )
{
    assert(! (phpctx->dwFlags & HPF_INTERRUPTED));
    assert(phpctx->cSuccess > 0);

    // HashCalcInitSave will set the file handle
    HashCalcInitSave(phpctx);

    if (phpctx->hFileOut != INVALID_HANDLE_VALUE)
    {
        // If the last item in the list already has the desired hash computed
        DWORD dwDesiredHash = 1 << (phpctx->ofn.nFilterIndex - 1);
        if (((PHASHPROPITEM)SLGetDataLast(phpctx->hList))->results.dwFlags & dwDesiredHash)
        {
            HashPropDoSaveResults(phpctx);
        }
        else
        {
            // Ensure the desired hash is enabled, and begin generating the new hash(es)
            assert(phpctx->status == CLEANUP_COMPLETED);
            assert(phpctx->cHandledMsgs >= phpctx->cSentMsgs);
            phpctx->opt.dwChecksums |= dwDesiredHash;
            HashPropRestart(phpctx);
            // HashPropDoSaveResults() is called when the worker thread posts a HM_WORKERTHREAD_DONE msg
        }
    }
}

VOID WINAPI HashPropDoSaveResults(PHASHPROPCONTEXT phpctx)
{
    assert(phpctx->hFileOut != INVALID_HANDLE_VALUE);

	if (! (phpctx->dwFlags & HPF_INTERRUPTED))
	{
        HashCalcSetSaveFormat(phpctx);

		PHASHPROPITEM pItem;

		SLReset(phpctx->hList);

		while (pItem = SLGetDataAndStep(phpctx->hList))
			HashCalcWriteResult(phpctx, pItem);
	}

	CloseHandle(phpctx->hFileOut);
    phpctx->hFileOut = INVALID_HANDLE_VALUE;
}

VOID WINAPI HashPropSaveResultsCleanup( PHASHPROPCONTEXT phpctx )
{
    if (phpctx->hFileOut != INVALID_HANDLE_VALUE)
    {
        // Don't keep partially generated checksum files
        BOOL bDeleted = HashCalcDeleteFileByHandle(phpctx->hFileOut);

		CloseHandle(phpctx->hFileOut);

        // Should only happen on Windows XP
        if (!bDeleted)
            DeleteFile(phpctx->ofn.lpstrFile);

        phpctx->hFileOut = INVALID_HANDLE_VALUE;
	}
}


VOID WINAPI HashPropOptions( PHASHPROPCONTEXT phpctx )
{
	OptionsDialog(phpctx->hWnd, &phpctx->opt);

    // Update the results, but only if a file save isn't in progress
    if (phpctx->opt.dwFlags & HCOF_CHECKSUMS && phpctx->hFileOut == INVALID_HANDLE_VALUE)
    {
        phpctx->dwFlags |= HCF_RESTARTING;
        WorkerThreadStop((PCOMMONCONTEXT)phpctx);
        WorkerThreadCleanup((PCOMMONCONTEXT)phpctx);

        if (phpctx->cHandledMsgs >= phpctx->cSentMsgs)
            HashPropRestart(phpctx);
        // Otherwise the call to HashPropRestart() is made following the
        // last pending HM_WORKERTHREAD_UPDATE message in HashPropDlgProc()
    }

	if (phpctx->opt.dwFlags & HCOF_FONT)
	{
		HFONT hFont = CreateFontIndirect(&phpctx->opt.lfFont);

		if (hFont)
		{
			SendDlgItemMessage(phpctx->hWnd, IDC_RESULTS, WM_SETFONT, (WPARAM)hFont, TRUE);
			if (phpctx->hFont) DeleteObject(phpctx->hFont);
			phpctx->hFont = hFont;
		}
	}
}

VOID WINAPI HashPropRestart( PHASHPROPCONTEXT phpctx )
{
    // Reset these flags back to the default
    phpctx->dwFlags &= ~(HCF_RESTARTING | HPF_INTERRUPTED);

    // Just reset the list if it's fully loaded, else reload it from scratch
    if (phpctx->dwFlags & HPF_HLIST_PREPPED)
        SLReset(phpctx->hList);
    else
    {
        SLRelease(phpctx->hList);
        phpctx->hList = SLCreateEx(TRUE);
        phpctx->cTotal = 0;
    }

    // Reset the GUI to its initial state
    EnableControl( phpctx->hWnd, IDC_SAVE,       FALSE);
    EnableControl( phpctx->hWnd, IDC_FIND_NEXT,  FALSE);
    EnableControl( phpctx->hWnd, IDC_SEARCHBOX,  FALSE);
    EnableControl( phpctx->hWnd, IDC_PROG_TOTAL, TRUE);
    EnableControl( phpctx->hWnd, IDC_PROG_FILE,  TRUE);
    EnableControl( phpctx->hWnd, IDC_PAUSE,      TRUE);
    EnableControl( phpctx->hWnd, IDC_STOP,       TRUE);
    SetDlgItemText(phpctx->hWnd, IDC_RESULTS,    TEXT(""));
    SetControlText(phpctx->hWnd, IDC_STATUSBOX,  IDS_HP_STATUSBOX);
    SetControlText(phpctx->hWnd, IDC_PAUSE,      IDS_HC_PAUSE);
    SetProgressBarPause((PCOMMONCONTEXT)phpctx,  PBST_NORMAL);
    SendMessage(phpctx->hWndPBFile,  PBM_SETPOS, 0, 0);
    SendMessage(phpctx->hWndPBTotal, PBM_SETPOS, 0, 0);

    phpctx->cSuccess = 0;
    phpctx->obScratch = 0;

    phpctx->hThread = CreateThreadCRT(NULL, phpctx);

    if (!phpctx->hThread)
        WorkerThreadCleanup((PCOMMONCONTEXT)phpctx);
}
