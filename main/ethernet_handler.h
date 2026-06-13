#ifndef ETHERNET_HANDLER_H
#define ETHERNET_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

void setupEthernet(void);
void *getIpAddr(void);

#ifdef __cplusplus
}
#endif

#endif // ETHERNET_HANDLER_H