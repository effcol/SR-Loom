// opentrack_udp.h -- OpenTrack UDP sender
// Protocol: 48 bytes, 6 doubles (X, Y, Z, Yaw, Pitch, Roll), little-endian,
// position in cm, rotation in degrees. Default port 4242. Fire-and-forget UDP --
// matches OpenTrack's "UDP over network" input source.
//
// Ported into SR Loom from the Simulated Reality OpenTrack Bridge
// (https://github.com/effcol/leia-track-app-XYZ, MIT). Stripped the
// destructor's identity-on-shutdown send since SR Loom drives the
// lifecycle explicitly.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

namespace srw
{
    #pragma pack(push, 1)
    struct TOpenTrackPacket {
        double X;
        double Y;
        double Z;
        double Yaw;
        double Pitch;
        double Roll;
    };
    #pragma pack(pop)
    static_assert(sizeof(TOpenTrackPacket) == 48, "OpenTrack packet must be 48 bytes");

    class OpenTrackSender
    {
        SOCKET sock_     = INVALID_SOCKET;
        sockaddr_in dst_ = {};
        bool ready_      = false;
        bool wsaUp_      = false;

    public:
        bool Init(const char* host = "127.0.0.1", int port = 4242)
        {
            WSADATA wsa{};
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
            wsaUp_ = true;

            sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock_ == INVALID_SOCKET) return false;

            // Non-blocking so a slow OpenTrack receiver never stalls the SR
            // runtime thread we run on.
            u_long nb = 1;
            ioctlsocket(sock_, FIONBIO, &nb);

            dst_.sin_family = AF_INET;
            dst_.sin_port   = htons(static_cast<u_short>(port));
            inet_pton(AF_INET, host, &dst_.sin_addr);

            ready_ = true;
            return true;
        }

        bool Send(double x, double y, double z,
                  double yaw, double pitch, double roll)
        {
            if (!ready_) return false;
            TOpenTrackPacket pkt{ x, y, z, yaw, pitch, roll };
            const int sent = sendto(sock_,
                                    reinterpret_cast<const char*>(&pkt),
                                    sizeof(pkt), 0,
                                    reinterpret_cast<sockaddr*>(&dst_),
                                    sizeof(dst_));
            return sent == sizeof(pkt);
        }

        void SendIdentity() { Send(0, 0, 0, 0, 0, 0); }

        void Shutdown()
        {
            if (sock_ != INVALID_SOCKET)
            {
                // Final identity packet so any OpenTrack consumer that latched
                // our last pose doesn't freeze the in-game view at a random
                // angle when we stop transmitting.
                SendIdentity();
                closesocket(sock_);
                sock_ = INVALID_SOCKET;
            }
            if (wsaUp_) { WSACleanup(); wsaUp_ = false; }
            ready_ = false;
        }

        bool IsReady() const { return ready_; }

        ~OpenTrackSender() { Shutdown(); }
    };
}
