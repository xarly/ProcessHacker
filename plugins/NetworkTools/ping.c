/*
 * Process Hacker Network Tools -
 *   Ping dialog
 *
 * Copyright (C) 2013 dmex
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "nettools.h"

#define WM_PING_UPDATE (WM_APP + 151)
#define IDC_PING_GRAPH (55050)
static RECT NormalGraphTextMargin = { 5, 5, 5, 5 };
static RECT NormalGraphTextPadding = { 3, 3, 3, 3 };

static HFONT InitializeFont(
    _In_ HWND hwndDlg
    )
{
    LOGFONT logFont = { 0 };
    HFONT fontHandle = NULL;
        
    logFont.lfHeight = -15;
    logFont.lfWeight = FW_MEDIUM;
    logFont.lfQuality = CLEARTYPE_QUALITY | ANTIALIASED_QUALITY;

    wcscpy_s(logFont.lfFaceName, _countof(logFont.lfFaceName),
        WindowsVersion > WINDOWS_XP ? L"Segoe UI" : L"MS Shell Dlg 2"
        );

    fontHandle = CreateFontIndirect(&logFont);

    if (fontHandle)
    {
        SendMessage(hwndDlg, WM_SETFONT, (WPARAM)fontHandle, FALSE);
        return fontHandle;
    }

    return NULL;
}

static VOID PhNetworkPingUpdateGraph(
    _In_ PNETWORK_OUTPUT_CONTEXT Context
    )
{
    Context->PingGraphState.Valid = FALSE;
    Context->PingGraphState.TooltipIndex = -1;
    Graph_MoveGrid(Context->PingGraphHandle, 1);
    Graph_Draw(Context->PingGraphHandle);
    Graph_UpdateTooltip(Context->PingGraphHandle);
    InvalidateRect(Context->PingGraphHandle, NULL, FALSE);
}

/**
 * Creates a Ansi string using format specifiers.
 *
 * \param Format The format-control string.
 * \param ArgPtr A pointer to the list of arguments.
 */
static PPH_ANSI_STRING PhFormatAnsiString_V(
    _In_ _Printf_format_string_ PSTR Format,
    _In_ va_list ArgPtr
    )
{
    PPH_ANSI_STRING string;
    int length;

    length = _vscprintf(Format, ArgPtr);

    if (length == -1)
        return NULL;

    string = PhCreateAnsiStringEx(NULL, length * sizeof(CHAR));

    _vsnprintf(
        string->Buffer, 
        length, 
        Format, ArgPtr
        );

    return string;
}

/**
 * Creates a Ansi string using format specifiers.
 *
 * \param Format The format-control string.
 */
static PPH_ANSI_STRING PhFormatAnsiString(
    _In_ _Printf_format_string_ PSTR Format,
    ...
    )
{
    va_list argptr;

    va_start(argptr, Format);

    return PhFormatAnsiString_V(Format, argptr);
}

static ULONG PhNetworkPingThreadStart(
    _In_ PVOID Parameter
    )
{
    HANDLE icmpHandle = INVALID_HANDLE_VALUE;
    ULONG icmpCurrentPingMs = 0;
    ULONG icmpCurrentPingTtl = 0;
    ULONG icmpReplyCount = 0;
    ULONG icmpReplyLength = 0;
    PVOID icmpReplyBuffer = NULL;
    PPH_STRING phVersion = NULL;
    PPH_ANSI_STRING icmpEchoBuffer = NULL;
    PNETWORK_OUTPUT_CONTEXT context = NULL;

    static IP_OPTION_INFORMATION pingOptions = 
    { 
        255,         // Time To Live
        0,           // Type Of Service
        IP_FLAG_DF,  // IP header flags
        0            // Size of options data
    };

    __try
    {
        // Query thread context.
        if ((context = (PNETWORK_OUTPUT_CONTEXT)Parameter) == NULL)
            __leave;

        // Query PH version.
        if ((phVersion = PhGetPhVersion()) == NULL)
            __leave;

        // Create ICMP echo buffer.
        if ((icmpEchoBuffer = PhFormatAnsiString("processhacker_%S_0x0D06F00D_x1", phVersion->Buffer)) == NULL)
            __leave;

        if (context->IpAddress.Type == PH_IPV6_NETWORK_TYPE)
        {          
            SOCKADDR_IN6 icmp6LocalAddr = { 0 };
            SOCKADDR_IN6 icmp6RemoteAddr = { 0 };
            PICMPV6_ECHO_REPLY icmp6ReplyStruct = NULL;

            // Create ICMPv6 handle.
            if ((icmpHandle = Icmp6CreateFile()) == INVALID_HANDLE_VALUE)
                __leave;

            // Set Local IPv6-ANY address.
            icmp6LocalAddr.sin6_addr = in6addr_any;
            icmp6LocalAddr.sin6_family = AF_INET6;

            // Set Remote IPv6 address.
            icmp6RemoteAddr.sin6_addr = context->IpAddress.In6Addr;
            icmp6RemoteAddr.sin6_port = _byteswap_ushort((USHORT)context->NetworkItem->RemoteEndpoint.Port);

            // Allocate ICMPv6 message.
            icmpReplyLength = ICMP_IPv6_BUFFER_SIZE(icmpEchoBuffer);
            icmpReplyBuffer = PhAllocate(icmpReplyLength);
            memset(icmpReplyBuffer, 0, icmpReplyLength);

            InterlockedIncrement(&context->PingSentCount);

            // Send ICMPv6 ping...
            icmpReplyCount = Icmp6SendEcho2(            
                icmpHandle,
                NULL,
                NULL,
                NULL, 
                &icmp6LocalAddr,
                &icmp6RemoteAddr,
                icmpEchoBuffer->Buffer, 
                icmpEchoBuffer->MaximumLength,
                &pingOptions,
                icmpReplyBuffer,
                icmpReplyLength,
                context->MaxPingTimeout
                );

            icmp6ReplyStruct = (PICMPV6_ECHO_REPLY)icmpReplyBuffer;
            if (icmpReplyCount > 0 && icmp6ReplyStruct)
            {
                BOOLEAN icmpPacketSignature = FALSE;

                if (icmp6ReplyStruct->Status != IP_SUCCESS)
                {
                    InterlockedIncrement(&context->PingLossCount);
                }

                if (_memicmp(
                    icmp6ReplyStruct->Address.sin6_addr, 
                    context->IpAddress.In6Addr.u.Word, 
                    sizeof(icmp6ReplyStruct->Address.sin6_addr)
                    ) != 0)
                {
                    InterlockedIncrement(&context->UnknownAddrCount);
                }

                //if (icmp6ReplyStruct->DataSize == icmpEchoBuffer->MaximumLength)
                //{
                //    icmpPacketSignature = (_memicmp(
                //        icmpEchoBuffer->Buffer, 
                //        icmp6ReplyStruct->Data, 
                //        icmp6ReplyStruct->DataSize
                //        ) == 0);
                //}

                //if (icmpPacketSignature != TRUE)
                //{
                //    InterlockedIncrement(&context->HashFailCount);
                //}

                icmpCurrentPingMs = icmp6ReplyStruct->RoundTripTime;
                //icmpCurrentPingTtl = icmp6ReplyStruct->Options.Ttl;
            }
            else
            {
                InterlockedIncrement(&context->PingLossCount);
            }
        }
        else
        {
            IPAddr icmpLocalAddr = 0;
            IPAddr icmpRemoteAddr = 0;
            PICMP_ECHO_REPLY icmpReplyStruct = NULL;
            
            // Create ICMPv4 handle.
            if ((icmpHandle = IcmpCreateFile()) == INVALID_HANDLE_VALUE)
                __leave;
            
            // Set Local IPv4-ANY address.
            icmpLocalAddr = in4addr_any.S_un.S_addr;

            // Set Remote IPv4 address.
            icmpRemoteAddr = context->IpAddress.InAddr.S_un.S_addr;

            // Allocate ICMPv4 message.
            icmpReplyLength = ICMP_IPv4_BUFFER_SIZE(icmpEchoBuffer);
            icmpReplyBuffer = PhAllocate(icmpReplyLength);
            memset(icmpReplyBuffer, 0, icmpReplyLength);
   
            InterlockedIncrement(&context->PingSentCount);

            // Send ICMPv4 ping...
            //if (WindowsVersion > WINDOWS_VISTA)
            //{
            //    // Vista SP1 and up we can specify the source address:
            //    icmpReplyCount = IcmpSendEcho2Ex(
            //        icmpHandle,
            //        NULL,
            //        NULL,
            //        NULL,
            //        icmpLocalAddr,
            //        icmpRemoteAddr,
            //        icmpEchoBuffer->Buffer, 
            //        icmpEchoBuffer->MaximumLength,
            //        &pingOptions,
            //        icmpReplyBuffer,
            //        icmpReplyLength,
            //        context->MaxPingTimeout
            //        );
            //}
            //else

            icmpReplyCount = IcmpSendEcho2(
                icmpHandle,
                NULL,
                NULL,
                NULL, 
                icmpRemoteAddr,
                icmpEchoBuffer->Buffer, 
                icmpEchoBuffer->MaximumLength,
                &pingOptions,
                icmpReplyBuffer,
                icmpReplyLength,
                context->MaxPingTimeout
                );

            icmpReplyStruct = (PICMP_ECHO_REPLY)icmpReplyBuffer;

            if (icmpReplyStruct && icmpReplyCount > 0)
            { 
                BOOLEAN icmpPacketSignature = FALSE;

                if (icmpReplyStruct->Status != IP_SUCCESS)
                {
                    InterlockedIncrement(&context->PingLossCount);
                }

                if (icmpReplyStruct->Address != context->IpAddress.InAddr.S_un.S_addr)
                {
                    InterlockedIncrement(&context->UnknownAddrCount);
                }
               
                if (icmpReplyStruct->DataSize == icmpEchoBuffer->MaximumLength)
                {
                    icmpPacketSignature = (_memicmp(
                        icmpEchoBuffer->Buffer, 
                        icmpReplyStruct->Data, 
                        icmpReplyStruct->DataSize
                        ) == 0);
                }

                icmpCurrentPingMs = icmpReplyStruct->RoundTripTime;
                icmpCurrentPingTtl = icmpReplyStruct->Options.Ttl;

                if (!icmpPacketSignature)
                {
                    InterlockedIncrement(&context->HashFailCount);
                }
            }
            else
            {
                InterlockedIncrement(&context->PingLossCount);
            }
        }

        InterlockedIncrement(&context->PingRecvCount);

        if (context->PingMinMs == 0 || icmpCurrentPingMs < context->PingMinMs)
            context->PingMinMs = icmpCurrentPingMs;
        if (icmpCurrentPingMs > context->PingMaxMs)
            context->PingMaxMs = icmpCurrentPingMs;

        context->CurrentPingMs = icmpCurrentPingMs;

        PhAddItemCircularBuffer_ULONG(&context->PingHistory, icmpCurrentPingMs);                
    }
    __finally
    {        
        if (phVersion)
        {
            PhDereferenceObject(phVersion);
        }

        if (icmpEchoBuffer)
        {
            PhDereferenceObject(icmpEchoBuffer);
        }

        if (icmpHandle)
        {
            IcmpCloseHandle(icmpHandle);
        }
    }

    return STATUS_SUCCESS;
}

static VOID NTAPI NetworkPingUpdateHandler(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PNETWORK_OUTPUT_CONTEXT context = (PNETWORK_OUTPUT_CONTEXT)Context;

    // Queue up the next ping into our work queue...
    PhQueueItemWorkQueue(
        &context->PingWorkQueue, 
        PhNetworkPingThreadStart, 
        (PVOID)context
        );

    PostMessage(context->WindowHandle, WM_PING_UPDATE, 0, 0);
}

static INT_PTR CALLBACK NetworkPingWndProc(
    _In_ HWND hwndDlg,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    PNETWORK_OUTPUT_CONTEXT context = NULL;

    if (uMsg == WM_INITDIALOG)
    {
        context = (PNETWORK_OUTPUT_CONTEXT)lParam;
        SetProp(hwndDlg, L"Context", (HANDLE)context);
    }
    else
    {
        context = (PNETWORK_OUTPUT_CONTEXT)GetProp(hwndDlg, L"Context");
        if (uMsg == WM_NCDESTROY)
        {
            PhSaveWindowPlacementToSetting(
                SETTING_NAME_PING_WINDOW_POSITION,
                SETTING_NAME_PING_WINDOW_SIZE,
                hwndDlg
                );

            PhDeleteWorkQueue(&context->PingWorkQueue);
            PhDeleteGraphState(&context->PingGraphState);
            PhDeleteLayoutManager(&context->LayoutManager);

            if (context->PingGraphHandle)
                DestroyWindow(context->PingGraphHandle);

            if (context->IconHandle)
                DestroyIcon(context->IconHandle);

            if (context->FontHandle)
                DeleteObject(context->FontHandle);

            RemoveProp(hwndDlg, L"Context");
            context = NULL;
        }
    }

    if (context == NULL)
        return FALSE;

    switch (uMsg)
    {
    case WM_INITDIALOG:
        {
            PH_RECTANGLE windowRectangle;
            PPH_LAYOUT_ITEM panelItem;

            // We have already set the group boxes to have WS_EX_TRANSPARENT to fix
            // the drawing issue that arises when using WS_CLIPCHILDREN. However
            // in removing the flicker from the graphs the group boxes will now flicker.
            // It's a good tradeoff since no one stares at the group boxes.
            PhSetWindowStyle(hwndDlg, WS_CLIPCHILDREN, WS_CLIPCHILDREN);

            context->WindowHandle = hwndDlg;
            context->ParentHandle = GetParent(hwndDlg);
            context->StatusHandle = GetDlgItem(hwndDlg, IDC_MAINTEXT);
            context->MaxPingTimeout = PhGetIntegerSetting(SETTING_NAME_PING_TIMEOUT);

            windowRectangle.Position = PhGetIntegerPairSetting(SETTING_NAME_PING_WINDOW_POSITION);
            windowRectangle.Size = PhGetIntegerPairSetting(SETTING_NAME_PING_WINDOW_SIZE);

            // Create the font handle.
            context->FontHandle = InitializeFont(context->StatusHandle);
        
            // Create the graph control.
            context->PingGraphHandle = CreateWindow(
                PH_GRAPH_CLASSNAME,
                NULL,
                WS_VISIBLE | WS_CHILD | WS_BORDER | WS_TABSTOP | GC_STYLE_DRAW_PANEL, // GC_STYLE_FADEOUT
                0,
                0,
                3,
                3,
                hwndDlg,
                (HMENU)IDC_PING_GRAPH,
                (HINSTANCE)PluginInstance->DllBase,
                NULL
                );

            // Load the Process Hacker icon.
            context->IconHandle = (HICON)LoadImage(
                GetModuleHandle(NULL),
                MAKEINTRESOURCE(PHAPP_IDI_PROCESSHACKER),
                IMAGE_ICON,
                GetSystemMetrics(SM_CXICON),
                GetSystemMetrics(SM_CYICON),
                LR_SHARED
                );
            // Set window icon.
            if (context->IconHandle) 
                SendMessage(hwndDlg, WM_SETICON, ICON_SMALL, (LPARAM)context->IconHandle);

            // Initialize the WorkQueue with a maximum of 20 threads (fix pinging slow-links with a high interval update).
            PhInitializeWorkQueue(&context->PingWorkQueue, 0, 20, 5000);
            PhInitializeGraphState(&context->PingGraphState);
            PhInitializeLayoutManager(&context->LayoutManager, hwndDlg);
            PhInitializeCircularBuffer_ULONG(&context->PingHistory, PhGetIntegerSetting(L"SampleCount"));

            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_ICMP_PANEL), NULL, PH_ANCHOR_BOTTOM | PH_ANCHOR_LEFT);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_ICMP_AVG), NULL, PH_ANCHOR_BOTTOM | PH_ANCHOR_LEFT);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_ICMP_MIN), NULL, PH_ANCHOR_BOTTOM | PH_ANCHOR_LEFT);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_ICMP_MAX), NULL, PH_ANCHOR_BOTTOM | PH_ANCHOR_LEFT);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_PINGS_SENT), NULL, PH_ANCHOR_BOTTOM | PH_ANCHOR_LEFT);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_PINGS_LOST), NULL, PH_ANCHOR_BOTTOM | PH_ANCHOR_LEFT);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_BAD_HASH), NULL, PH_ANCHOR_BOTTOM | PH_ANCHOR_LEFT);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_ANON_ADDR), NULL, PH_ANCHOR_BOTTOM | PH_ANCHOR_LEFT);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDOK), NULL, PH_ANCHOR_BOTTOM | PH_ANCHOR_RIGHT);        
            panelItem = PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_PING_LAYOUT), NULL, PH_ANCHOR_ALL);      
            PhAddLayoutItemEx(&context->LayoutManager, context->PingGraphHandle, NULL, PH_ANCHOR_ALL, panelItem->Margin);

            // Load window settings.
            if (windowRectangle.Position.X == 0 || windowRectangle.Position.Y == 0)
                PhCenterWindow(hwndDlg, GetParent(hwndDlg));
            else
            {
                PhLoadWindowPlacementFromSetting(SETTING_NAME_PING_WINDOW_POSITION, SETTING_NAME_PING_WINDOW_SIZE, hwndDlg);
            }

            // Initialize window layout.
            PhLayoutManagerLayout(&context->LayoutManager);

            // Convert IP Address to string format.
            if (context->IpAddress.Type == PH_IPV4_NETWORK_TYPE)
            {
                RtlIpv4AddressToString(
                    &context->IpAddress.InAddr, 
                    context->addressString
                    );
            }
            else
            {
                RtlIpv6AddressToString(
                    &context->IpAddress.In6Addr, 
                    context->addressString
                    );
            }

            SetWindowText(hwndDlg, PhaFormatString(L"Ping %s", context->addressString)->Buffer);
            SetWindowText(context->StatusHandle, PhaFormatString(L"Pinging %s with 32 bytes of data:", context->addressString)->Buffer);
            Graph_SetTooltip(context->PingGraphHandle, TRUE);

            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackProcessesUpdated),
                NetworkPingUpdateHandler,
                context,
                &context->ProcessesUpdatedRegistration
                );
        }
        return TRUE;
    case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
            case IDCANCEL:
            case IDOK:
                PostQuitMessage(0);
                break;
            }
        }
        break;
    case WM_DESTROY:
        {
            PhUnregisterCallback(
                PhGetGeneralCallback(GeneralCallbackProcessesUpdated), 
                &context->ProcessesUpdatedRegistration
                );
        }
        break;
    case WM_SIZE:
        PhLayoutManagerLayout(&context->LayoutManager);
        break;
    case WM_SIZING:
        PhResizingMinimumSize((PRECT)lParam, wParam, 420, 250);
        break;
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        {
            HDC hDC = (HDC)wParam;
            HWND hwndChild = (HWND)lParam;

            // Check for our static label and change the color.
            if (GetDlgCtrlID(hwndChild) == IDC_MAINTEXT)
            {
                SetTextColor(hDC, RGB(19, 112, 171));
            }

            // Set a transparent background for the control backcolor.
            SetBkMode(hDC, TRANSPARENT);

            // set window background color.
            return (INT_PTR)GetSysColorBrush(COLOR_WINDOW);
        }       
        break;
    case WM_PING_UPDATE:
        {
            ULONG i = 0;
            ULONG maxGraphHeight = 0;
            ULONG pingAvgValue = 0;

            PhNetworkPingUpdateGraph(context);

            for (i = 0; i < context->PingHistory.Count; i++)
            {
                maxGraphHeight = maxGraphHeight + PhGetItemCircularBuffer_ULONG(&context->PingHistory, i);
                pingAvgValue = maxGraphHeight / context->PingHistory.Count;
            }

            SetDlgItemText(hwndDlg, IDC_ICMP_AVG, PhaFormatString(
                L"Average: %ums", pingAvgValue)->Buffer);
            SetDlgItemText(hwndDlg, IDC_ICMP_MIN, PhaFormatString(
                L"Minimum: %ums", context->PingMinMs)->Buffer);
            SetDlgItemText(hwndDlg, IDC_ICMP_MAX, PhaFormatString(
                L"Maximum: %ums", context->PingMaxMs)->Buffer);
                        
            SetDlgItemText(hwndDlg, IDC_PINGS_SENT, PhaFormatString(
                L"Pings Sent: %u", context->PingSentCount)->Buffer);
            SetDlgItemText(hwndDlg, IDC_PINGS_LOST, PhaFormatString(
                L"Pings Lost: %u (%.0f%%)", context->PingLossCount, 
                ((FLOAT)context->PingLossCount / context->PingSentCount * 100)
                )->Buffer);

            SetDlgItemText(hwndDlg, IDC_BAD_HASH, PhaFormatString(
                L"Bad Hashes: %u", context->HashFailCount)->Buffer);  
            SetDlgItemText(hwndDlg, IDC_ANON_ADDR, PhaFormatString(
                L"Anon Replies: %u", context->UnknownAddrCount)->Buffer);   
        }
        break;
    case WM_NOTIFY:
        {
            LPNMHDR header = (LPNMHDR)lParam;

            switch (header->code)
            {
            case GCN_GETDRAWINFO:
                {
                    PPH_GRAPH_GETDRAWINFO getDrawInfo = (PPH_GRAPH_GETDRAWINFO)header;
                    PPH_GRAPH_DRAW_INFO drawInfo = getDrawInfo->DrawInfo;

                    PhSiSetColorsGraphDrawInfo(drawInfo, PhGetIntegerSetting(L"ColorCpuKernel"), PhGetIntegerSetting(L"ColorCpuUser"));

                    if (header->hwndFrom == context->PingGraphHandle)
                    {
                        if (PhGetIntegerSetting(L"GraphShowText"))
                        {
                            HDC hdc = Graph_GetBufferedContext(context->PingGraphHandle);

                            PhSwapReference2(&context->PingGraphState.Text, 
                                PhFormatString(L"Ping: %ums", context->CurrentPingMs)
                                );

                            SelectObject(hdc, PhApplicationFont);
                            PhSetGraphText(hdc, drawInfo, &context->PingGraphState.Text->sr,
                                &NormalGraphTextMargin, &NormalGraphTextPadding, PH_ALIGN_TOP | PH_ALIGN_LEFT);
                        }
                        else
                        {
                            drawInfo->Text.Buffer = NULL;
                        }
                  
                        PhGraphStateGetDrawInfo(
                            &context->PingGraphState,
                            getDrawInfo,
                            context->PingHistory.Count
                            );
            
                        if (!context->PingGraphState.Valid)
                        {
                            ULONG i = 0;
                            //FLOAT maxGraphHeight = 0;

                            for (i = 0; i < drawInfo->LineDataCount; i++)
                            {
                                context->PingGraphState.Data1[i] =
                                    (FLOAT)PhGetItemCircularBuffer_ULONG(&context->PingHistory, i);
                                //if (maxGraphHeight == 0)
                                //    maxGraphHeight = context->PingGraphState.Data1[i];
                                //if (context->PingGraphState.Data1[i] > maxGraphHeight)
                                //    maxGraphHeight = context->PingGraphState.Data1[i];
                            }

                            // Scale the data.
                            PhxfDivideSingle2U(
                                context->PingGraphState.Data1,
                                (FLOAT)context->MaxPingTimeout, // maxGraphHeight
                                drawInfo->LineDataCount
                                );

                            context->PingGraphState.Valid = TRUE;
                        }
                    }
                }
                break;
            case GCN_GETTOOLTIPTEXT:
                {
                    PPH_GRAPH_GETTOOLTIPTEXT getTooltipText = (PPH_GRAPH_GETTOOLTIPTEXT)lParam;

                    if (getTooltipText->Index < getTooltipText->TotalCount)
                    {
                        if (header->hwndFrom == context->PingGraphHandle)
                        {
                            if (context->PingGraphState.TooltipIndex != getTooltipText->Index)
                            {
                                ULONG pingMs = PhGetItemCircularBuffer_ULONG(&context->PingHistory, getTooltipText->Index);

                                PhSwapReference2(&context->PingGraphState.TooltipText, 
                                    PhFormatString(L"Ping: %ums", pingMs)
                                    );
                            }

                            getTooltipText->Text = context->PingGraphState.TooltipText->sr;
                        }
                    }
                }
                break;
            }
        }
        break;
    }

    return FALSE;
}

NTSTATUS PhNetworkPingDialogThreadStart(
    _In_ PVOID Parameter
    )
{
    BOOL result;
    MSG message;
    HWND windowHandle;
    PH_AUTO_POOL autoPool;
    PNETWORK_OUTPUT_CONTEXT context = (PNETWORK_OUTPUT_CONTEXT)Parameter;

    PhInitializeAutoPool(&autoPool);

    windowHandle = CreateDialogParam(
        (HINSTANCE)PluginInstance->DllBase,
        MAKEINTRESOURCE(IDD_PINGDIALOG),
        PhMainWndHandle,
        NetworkPingWndProc,
        (LPARAM)Parameter
        );

    ShowWindow(windowHandle, SW_SHOW);
    SetForegroundWindow(windowHandle);

    while (result = GetMessage(&message, NULL, 0, 0))
    {
        if (result == -1)
            break;

        if (!IsDialogMessage(windowHandle, &message))
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }

        PhDrainAutoPool(&autoPool);
    }

    PhDeleteAutoPool(&autoPool);
    DestroyWindow(windowHandle);

    PhFree(context);

    return STATUS_SUCCESS;
}