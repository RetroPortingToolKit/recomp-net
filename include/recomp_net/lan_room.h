#ifndef RECOMP_NET_LAN_ROOM_H
#define RECOMP_NET_LAN_ROOM_H
#include "recomp_net/lan_lobby.h"
#include "recomp_net/lan_direct.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Explicitly opted-in 2..4-seat waiting rooms. The existing two-seat
 * lan_lobby/lan_direct APIs and wire remain available to older consumers.
 * One authority, stable random member IDs, revisioned complete rosters and
 * idempotent sequenced requests. Only the host may change caps/start/kick.
 * Not thread-safe: pump and operate on the launcher's owning thread. */
#define RNET_LAN_ROOM_MAX 4
typedef struct RNetLanRoomMember {
    rnet_u32 id; /* 0 means empty; names are never identities */
    char name[RNET_LAN_LOBBY_PLAYER_MAX];
} RNetLanRoomMember;
typedef struct RNetLanRoom {
    char name[RNET_LAN_LOBBY_NAME_MAX];
    char game[RNET_LAN_LOBBY_GAME_MAX];
    char game_version[RNET_LAN_LOBBY_VERSION_MAX];
    char endpoint[RNET_LAN_LOBBY_ENDPOINT_MAX];
    char mod_set[RNET_LAN_LOBBY_MOD_SET_MAX];
    rnet_u32 epoch,revision,configuration,session_id,ready_mask;
    int max_slots,host_slot,input_delay,rollback,input_prediction,session_variant,starting;
    RNetLanRoomMember member[RNET_LAN_ROOM_MAX];
} RNetLanRoom;
typedef struct RNetLanRoomPeer RNetLanRoomPeer;
enum {
    RNET_LAN_ROOM_OK=0, RNET_LAN_ROOM_PENDING=1,
    RNET_LAN_ROOM_IO=-1, RNET_LAN_ROOM_FULL=-2,
    RNET_LAN_ROOM_PASSWORD=-3, RNET_LAN_ROOM_IDENTITY=-4,
    RNET_LAN_ROOM_STARTED=-5, RNET_LAN_ROOM_TIMEOUT=-6,
    RNET_LAN_ROOM_ARGUMENT=-7, RNET_LAN_ROOM_STALE=-8,
    RNET_LAN_ROOM_NOT_READY=-9, RNET_LAN_ROOM_KICKED=-10,
    RNET_LAN_ROOM_CLOSED=-11, RNET_LAN_ROOM_OCCUPIED=-12
};
/* Host config supplies identity/caps/seat/name; epoch/revision/IDs are owned
 * by the protocol. Password is private and never included in a roster. */
int rnet_lan_room_host_open(RNetLanRoomPeer**,const char* bind,const RNetLanRoom*,const char* password);
int rnet_lan_room_guest_open(RNetLanRoomPeer**,const char* bind,const char* host,
                             const char* game,const char* version,const char* password,
                             const char* name,int supported_slots);
/* Nonblocking; returns current status (pending joining, okay, or latched
 * terminal error). Continue pumping held rooms and while start is pending. */
int rnet_lan_room_pump(RNetLanRoomPeer*);
void rnet_lan_room_close(RNetLanRoomPeer**);
const RNetLanRoom* rnet_lan_room_state(const RNetLanRoomPeer*);
int rnet_lan_room_local_slot(const RNetLanRoomPeer*);
int rnet_lan_room_is_host(const RNetLanRoomPeer*);
int rnet_lan_room_all_ready(const RNetLanRoomPeer*);
int rnet_lan_room_set_ready(RNetLanRoomPeer*,int ready);
/* Only empty destinations are claimed. Occupied-seat trades use the older
 * two-seat API until an explicit consent protocol is supplied here. */
int rnet_lan_room_move(RNetLanRoomPeer*,int from,int to);
int rnet_lan_room_kick(RNetLanRoomPeer*,int slot);
int rnet_lan_room_set_caps(RNetLanRoomPeer*,int delay,int rollback,int prediction,
                           int variant,const char* mod_set);
/* Allocate a fresh session ID and freeze a fully ready roster. Start is
 * retransmitted and acknowledged by every member before host launch_ready.
 * Guests retain their listener briefly and re-ACK retries before handoff.
 * Loss beyond the bounded handshake is a refusal, never a smaller match. */
int rnet_lan_room_start(RNetLanRoomPeer*);
int rnet_lan_room_launch_ready(const RNetLanRoomPeer*);
int rnet_lan_room_member_rtt(const RNetLanRoomPeer*,int slot);
/* Waiting-room chat follows the existing best-effort LAN chat contract:
 * the host supplies the sender's name and ordering, including its own lines.
 * It has no account/report identity and is separate from simulation input. */
int rnet_lan_room_send_chat(RNetLanRoomPeer*,const char* text);
int rnet_lan_room_take_chat(RNetLanRoomPeer*,RNetLanChatLine* out);
#ifdef __cplusplus
}
#endif
#endif
