#ifndef _UINET_IF_PACHAOS_H_
#define _UINET_IF_PACHAOS_H_

struct uinet_if;

int if_pachaos_attach(struct uinet_if *uif);
int if_pachaos_detach(struct uinet_if *uif);

#endif
