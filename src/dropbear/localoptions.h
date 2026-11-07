/* Dropbear build options for the Buddy3D camera.
 * Public-key auth only; keys and host keys live on the SD card and are passed
 * on the command line (-r hostkey, -D authorized_keys dir) by lp_app.sh. */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_X11FWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_CLI_AGENTFWD 0
#define DROPBEAR_DSS 0
#define DROPBEAR_RSA 1
#define DROPBEAR_ECDSA 1
#define DROPBEAR_ED25519 1
#define DO_MOTD 0
#define DROPBEAR_SFTPSERVER 0
/* Camera tools live in /oem; keep the stock PATH plus our SD tools dir. */
#define DEFAULT_ROOT_PATH "/tmp/buddy3d/bin:/usr/sbin:/usr/bin:/sbin:/bin:/oem/usr/bin:/oem/usr/sbin"
#define DEFAULT_PATH DEFAULT_ROOT_PATH
