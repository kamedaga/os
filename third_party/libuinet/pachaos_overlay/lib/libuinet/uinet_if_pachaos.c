/*
 * PachaOS in-process interface for libuinet.
 *
 * This file is an integration shim owned by CapabilityOS. It follows the
 * libuinet interface-driver shape used by pcap/netmap, but receives frames
 * from netd directly instead of spawning a host capture thread.
 */

#include <sys/param.h>
#include <sys/ctype.h>
#include <sys/libkern.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/systm.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_types.h>
#include <net/if_var.h>

#include "uinet_internal.h"
#include "uinet_pachaos_api.h"
#include "uinet_if_pachaos.h"

static void if_pachaos_default_config(union uinet_if_type_cfg *cfg);

static struct uinet_if_type_info if_pachaos_type_info = {
	.type = UINET_IFTYPE_PACHAOS,
	.type_name = "pachaos",
	.default_cfg = if_pachaos_default_config
};
UINET_IF_REGISTER_TYPE(PACHAOS, &if_pachaos_type_info);

struct if_pachaos_softc {
	struct ifnet *ifp;
	struct uinet_if *uif;
	uint8_t addr[ETHER_ADDR_LEN];
	uinet_pachaos_tx_func_t tx;
	void *tx_arg;
	struct uinet_pd_list_single rx_one;
};

static unsigned int interface_count;

static void
if_pachaos_default_config(union uinet_if_type_cfg *cfg)
{
	static const uint8_t default_mac[ETHER_ADDR_LEN] = {
		0x52, 0x54, 0x00, 0x12, 0x34, 0x56
	};

	memcpy(cfg->pachaos.mac, default_mac, sizeof(default_mac));
	cfg->pachaos.mtu = ETHERMTU;
}

static void
if_pachaos_init(void *arg)
{
	struct if_pachaos_softc *sc = arg;

	sc->ifp->if_drv_flags |= IFF_DRV_RUNNING;
	sc->ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;
}

static void
if_pachaos_stop(struct if_pachaos_softc *sc)
{
	sc->ifp->if_drv_flags &= ~(IFF_DRV_RUNNING | IFF_DRV_OACTIVE);
}

static int
if_pachaos_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct if_pachaos_softc *sc = ifp->if_softc;

	switch (cmd) {
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if_pachaos_init(sc);
		} else if (ifp->if_drv_flags & IFF_DRV_RUNNING) {
			if_pachaos_stop(sc);
		}
		return (0);
	default:
		return ether_ioctl(ifp, cmd, data);
	}
}

static int
if_pachaos_transmit(struct ifnet *ifp, struct mbuf *m)
{
	struct if_pachaos_softc *sc = ifp->if_softc;
	unsigned char frame[MCLBYTES];
	int error = 0;
	int len;

	if (sc->tx == NULL) {
		error = ENETUNREACH;
		goto out;
	}

	len = m->m_pkthdr.len;
	if (len <= 0 || len > (int)sizeof(frame)) {
		error = EMSGSIZE;
		goto out;
	}

	m_copydata(m, 0, len, (caddr_t)frame);
	error = sc->tx(sc->tx_arg, frame, (size_t)len);
	if (error == 0) {
		ifp->if_opackets++;
		ifp->if_obytes += len;
	} else {
		ifp->if_oerrors++;
	}

out:
	m_freem(m);
	return (error);
}

static void
if_pachaos_pd_alloc_user(struct uinet_if *uif __unused, struct uinet_pd_list *pkts)
{
	uint32_t alloc_size = pkts->num_descs;

	pkts->num_descs = 0;
	uinet_pd_mbuf_alloc_descs(pkts, alloc_size);
}

static void
if_pachaos_inject_tx_pkts(struct uinet_if *uif, struct uinet_pd_list *pkts)
{
	struct if_pachaos_softc *sc = uif->ifdata;
	struct uinet_pd_ctx *to_free[UINET_PD_FREE_BATCH_SIZE];
	unsigned int to_free_count = 0;
	uint32_t i;

	for (i = 0; i < pkts->num_descs; i++) {
		struct uinet_pd *pd = &pkts->descs[i];

		if ((pd->flags & UINET_PD_INJECT) && sc->tx != NULL) {
			(void)sc->tx(sc->tx_arg, pd->data, pd->length);
		}

		to_free[to_free_count++] = pd->ctx;
		if (to_free_count == UINET_PD_FREE_BATCH_SIZE) {
			uinet_pd_ref_release(to_free, to_free_count);
			to_free_count = 0;
		}
	}

	if (to_free_count != 0) {
		uinet_pd_ref_release(to_free, to_free_count);
	}
	pkts->num_descs = 0;
}

static int
if_pachaos_batch_noop(struct uinet_if *uif __unused, int *fd, uint64_t *wait_ns)
{
	*fd = -1;
	*wait_ns = 1000000000;
	return (0);
}

int
if_pachaos_attach(struct uinet_if *uif)
{
	struct if_pachaos_softc *sc;
	struct ifnet *ifp;
	unsigned int mtu;

	snprintf(uif->name, sizeof(uif->name), "pachaos%u", interface_count);
	interface_count++;

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
	if (sc == NULL) {
		return (ENOMEM);
	}

	sc->uif = uif;
	memcpy(sc->addr, uif->type_cfg.pachaos.mac, sizeof(sc->addr));
	mtu = uif->type_cfg.pachaos.mtu;
	if (mtu == 0) {
		mtu = ETHERMTU;
	}

	ifp = sc->ifp = if_alloc(IFT_ETHER);
	if (ifp == NULL) {
		free(sc, M_DEVBUF);
		return (ENOMEM);
	}

	ifp->if_softc = sc;
	ifp->if_init = if_pachaos_init;
	if_initname(ifp, uif->name, IF_DUNIT_NONE);
	ifp->if_mtu = mtu;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = if_pachaos_ioctl;
	ifp->if_transmit = if_pachaos_transmit;

	IFQ_SET_MAXLEN(&ifp->if_snd, 1024);
	ifp->if_snd.ifq_drv_maxlen = 1024;
	IFQ_SET_READY(&ifp->if_snd);

	ether_ifattach(ifp, sc->addr);
	ifp->if_capabilities = ifp->if_capenable = 0;

	uif->pd_alloc = if_pachaos_pd_alloc_user;
	uif->inject_tx_pkts = if_pachaos_inject_tx_pkts;
	uif->batch_rx = if_pachaos_batch_noop;
	uif->batch_tx = if_pachaos_batch_noop;
	uinet_if_attach(uif, ifp, sc);
	if_pachaos_init(sc);
	return (0);
}

int
if_pachaos_detach(struct uinet_if *uif)
{
	struct if_pachaos_softc *sc = uif->ifdata;

	if (sc != NULL) {
		if (sc->ifp != NULL) {
			ether_ifdetach(sc->ifp);
			if_free(sc->ifp);
		}
		free(sc, M_DEVBUF);
	}
	return (0);
}

int
uinet_pachaos_if_register_tx(uinet_if_t uif, uinet_pachaos_tx_func_t tx, void *arg)
{
	struct if_pachaos_softc *sc;

	if (uif == NULL || uif->type != UINET_IFTYPE_PACHAOS || uif->ifdata == NULL) {
		return (EINVAL);
	}

	sc = uif->ifdata;
	sc->tx = tx;
	sc->tx_arg = arg;
	return (0);
}

int
uinet_pachaos_if_deliver(uinet_if_t uif, const void *frame, size_t frame_len)
{
	struct if_pachaos_softc *sc;
	struct uinet_pd *pd;
	struct uinet_pd_list_single *rx_one;

	if (uif == NULL || frame == NULL || frame_len == 0 ||
	    uif->type != UINET_IFTYPE_PACHAOS || uif->ifdata == NULL) {
		return (EINVAL);
	}
	if (frame_len > MCLBYTES) {
		return (EMSGSIZE);
	}

	sc = uif->ifdata;
	if ((sc->ifp->if_drv_flags & IFF_DRV_RUNNING) == 0) {
		return (ENETDOWN);
	}

	rx_one = &sc->rx_one;
	rx_one->num_descs = 0;
	if (uinet_pd_mbuf_alloc_descs((struct uinet_pd_list *)rx_one, 1) != 1) {
		sc->ifp->if_ierrors++;
		return (ENOBUFS);
	}

	pd = &rx_one->descs[0];
	memcpy(pd->data, frame, frame_len);
	pd->length = (uint16_t)frame_len;
	pd->flags |= UINET_PD_TO_STACK;

	UIF_TIMESTAMP(uif, (struct uinet_pd_list *)rx_one);
	UIF_BATCH_EVENT(uif, UINET_BATCH_EVENT_START);
	UIF_FIRST_LOOK(uif, (struct uinet_pd_list *)rx_one);
	uinet_pd_deliver_to_stack(uif, (struct uinet_pd_list *)rx_one);
	UIF_BATCH_EVENT(uif, UINET_BATCH_EVENT_FINISH);

	sc->ifp->if_ipackets++;
	sc->ifp->if_ibytes += frame_len;
	rx_one->num_descs = 0;
	return (0);
}
