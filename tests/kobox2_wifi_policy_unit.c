/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "boot/wifi_service.h"

int main(void)
{
    static const char good[] = "ssid=fixture\nwpa_state=COMPLETED\naddress=00:11:22:33:44:55\n";
    static const char down[] = "wpa_state=DISCONNECTED\n";
    static const char forged[] = "ssid=wpa_state=COMPLETED\n";
    static const char duplicate[] = "wpa_state=DISCONNECTED\nwpa_state=COMPLETED\n";
    static const char suffix[] = "wpa_state=COMPLETED_NOT\n";
    assert(kobox_wifi_status_completed(good, sizeof(good) - 1) == 1);
    assert(kobox_wifi_status_completed(down, sizeof(down) - 1) == 0);
    assert(kobox_wifi_status_completed(forged, sizeof(forged) - 1) == -EPROTO);
    assert(kobox_wifi_status_completed(duplicate, sizeof(duplicate) - 1) == -EPROTO);
    assert(kobox_wifi_status_completed(suffix, sizeof(suffix) - 1) == 0);
    assert(kobox_wifi_status_completed(good, sizeof(good)) == -EPROTO);
    assert(kobox_wifi_status_completed(NULL, 0) == -EPROTO);

    char request[4097];
    size_t used = 0;
    assert(kobox_wifi_qualify_request("wlp88s0", "STATUS", 6,
        request, sizeof(request), &used) == 0);
    assert(!strcmp(request, "IFNAME=wlp88s0 STATUS") && used == strlen(request));
    assert(kobox_wifi_qualify_request("wlp88s0", "DISCONNECT", 10,
        request, sizeof(request), &used) == 0);
    assert(!strcmp(request, "IFNAME=wlp88s0 DISCONNECT"));
    assert(kobox_wifi_qualify_request("wlp88s0", "IFNAME=wlan99 STATUS", 20,
        request, sizeof(request), &used) == -EINVAL);
    assert(kobox_wifi_qualify_request("wlp88s0", "STATUS\nTERMINATE", 16,
        request, sizeof(request), &used) == -EINVAL);
    assert(kobox_wifi_qualify_request("wlp88s0", "STATUS", 7,
        request, sizeof(request), &used) == -EINVAL);
    assert(kobox_wifi_qualify_request("wlp88s0", "STATUS", 6,
        request, 8, &used) == -EINVAL);

    unsigned char frame[64] = {0};
    frame[12] = 0x08; frame[13] = 0x00;
    assert(kobox_wifi_data_frame_allowed(frame, sizeof(frame)));
    frame[12] = 0x88; frame[13] = 0x8e;
    assert(!kobox_wifi_data_frame_allowed(frame, sizeof(frame)));
    frame[12] = 0x81; frame[13] = 0x00;
    frame[16] = 0x88; frame[17] = 0x8e;
    assert(!kobox_wifi_data_frame_allowed(frame, sizeof(frame)));
    assert(!kobox_wifi_data_frame_allowed(frame, 17));
    frame[12] = 0x88; frame[13] = 0xa8;
    frame[16] = 0x81; frame[17] = 0x00;
    frame[20] = 0x88; frame[21] = 0x8e;
    assert(!kobox_wifi_data_frame_allowed(frame, sizeof(frame)));
    frame[20] = 0x86; frame[21] = 0xdd;
    assert(kobox_wifi_data_frame_allowed(frame, sizeof(frame)));
    frame[20] = 0x81; frame[21] = 0x00;
    frame[24] = 0x08; frame[25] = 0x00;
    assert(!kobox_wifi_data_frame_allowed(frame, sizeof(frame)));
    assert(!kobox_wifi_data_frame_allowed(NULL, 64));
    assert(!kobox_wifi_data_frame_allowed(frame, 13));
    puts("WiFi policy: exact COMPLETED state, real interface qualification and direct/VLAN EAPOL exclusion PASS");
    return 0;
}
