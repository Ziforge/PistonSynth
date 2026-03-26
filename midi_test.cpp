// Quick CoreMIDI test sender for Piston Synth
// Creates a virtual MIDI source and sends notes + CC
// The standalone app should see it as a MIDI input
//
// Build: clang++ -std=c++17 -framework CoreMIDI -framework CoreFoundation -o midi_test midi_test.cpp

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>

MIDIClientRef client;
MIDIEndpointRef source;

void sendBytes(const uint8_t* data, int len) {
    uint8_t buf[256];
    MIDIPacketList* pktList = (MIDIPacketList*)buf;
    MIDIPacket* pkt = MIDIPacketListInit(pktList);
    pkt = MIDIPacketListAdd(pktList, sizeof(buf), pkt, 0, len, data);
    if (pkt) MIDIReceived(source, pktList);
}

void noteOn(int ch, int note, int vel) {
    uint8_t msg[3] = {(uint8_t)(0x90|(ch-1)), (uint8_t)note, (uint8_t)vel};
    sendBytes(msg, 3);
}

void noteOff(int ch, int note) {
    uint8_t msg[3] = {(uint8_t)(0x80|(ch-1)), (uint8_t)note, 0};
    sendBytes(msg, 3);
}

void sendCC(int ch, int cc, int val) {
    uint8_t msg[3] = {(uint8_t)(0xB0|(ch-1)), (uint8_t)cc, (uint8_t)val};
    sendBytes(msg, 3);
}

void pitchBend(int ch, int value) {
    // value: 0-16383, center=8192
    uint8_t msg[3] = {(uint8_t)(0xE0|(ch-1)), (uint8_t)(value&0x7F), (uint8_t)((value>>7)&0x7F)};
    sendBytes(msg, 3);
}

int main() {
    OSStatus err = MIDIClientCreate(CFSTR("PistonSynthTest"), nullptr, nullptr, &client);
    if (err) { printf("Failed to create MIDI client: %d\n", err); return 1; }

    err = MIDISourceCreate(client, CFSTR("Piston Synth Test"), &source);
    if (err) { printf("Failed to create MIDI source: %d\n", err); return 1; }

    printf("=== Piston Synth MIDI Test ===\n");
    printf("Virtual MIDI source created: 'Piston Synth Test'\n");
    printf("Select this as MIDI input in the standalone app (Options menu)\n\n");
    printf("Press ENTER when the app is configured to receive...\n");
    getchar();

    printf("Sending test sequence...\n\n");

    // Test 1: Single notes ascending — different RPMs
    printf("1. Ascending notes (RPM sweep): D2 → D3 → D4 → D5\n");
    int notes[] = {38, 50, 62, 74};
    for (int n : notes) {
        printf("   Note %d (RPM ~%.0f)\n", n, 300.0 * pow(2.0, (n-36)/12.0));
        noteOn(1, n, 100);
        usleep(800000);
        noteOff(1, n);
        usleep(200000);
    }

    printf("\n2. Velocity dynamics: same note, soft → hard\n");
    int vels[] = {30, 60, 90, 127};
    for (int v : vels) {
        printf("   Velocity %d\n", v);
        noteOn(1, 50, v);
        usleep(600000);
        noteOff(1, 50);
        usleep(200000);
    }

    printf("\n3. CC16 (RPM knob) sweep while holding note...\n");
    noteOn(1, 55, 100);
    for (int i = 0; i <= 127; i += 8) {
        sendCC(1, 16, i);
        usleep(100000);
    }
    noteOff(1, 55);
    usleep(300000);

    printf("4. CC17 (Fuel) sweep...\n");
    noteOn(1, 55, 100);
    for (int i = 0; i <= 127; i += 8) {
        sendCC(1, 17, i);
        usleep(100000);
    }
    noteOff(1, 55);
    usleep(300000);

    printf("5. CC18 (Turbo) sweep...\n");
    noteOn(1, 55, 100);
    for (int i = 0; i <= 127; i += 8) {
        sendCC(1, 18, i);
        usleep(100000);
    }
    noteOff(1, 55);
    usleep(300000);

    printf("6. CC20 (Exhaust Cutoff) sweep...\n");
    noteOn(1, 55, 100);
    for (int i = 0; i <= 127; i += 8) {
        sendCC(1, 20, i);
        usleep(100000);
    }
    noteOff(1, 55);
    usleep(300000);

    printf("7. Pitch bend sweep...\n");
    noteOn(1, 55, 100);
    for (int i = 0; i <= 16383; i += 512) {
        pitchBend(1, i);
        usleep(80000);
    }
    pitchBend(1, 8192); // Center
    noteOff(1, 55);
    usleep(300000);

    printf("8. Mod wheel (vibrato) test...\n");
    noteOn(1, 60, 100);
    usleep(500000);
    for (int i = 0; i <= 127; i += 4) {
        sendCC(1, 1, i);
        usleep(50000);
    }
    usleep(1000000);
    sendCC(1, 1, 0);
    noteOff(1, 60);
    usleep(300000);

    printf("9. Chord: D minor (D3 + F3 + A3)\n");
    noteOn(1, 50, 90);  // D3
    noteOn(1, 53, 85);  // F3
    noteOn(1, 57, 80);  // A3
    usleep(2000000);
    noteOff(1, 50);
    noteOff(1, 53);
    noteOff(1, 57);
    usleep(500000);

    printf("\nDone! All tests complete.\n");

    MIDIEndpointDispose(source);
    MIDIClientDispose(client);
    return 0;
}
