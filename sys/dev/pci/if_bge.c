/*	$OpenBSD: if_bge.c,v 1.4 2001/12/24 18:30:43 mickey Exp $	*/
/*
 * Copyright (c) 2001 Wind River Systems
 * Copyright (c) 1997, 1998, 1999, 2001
 *	Bill Paul <wpaul@windriver.com>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/dev/bge/if_bge.c,v 1.2 2001/09/28 18:56:57 wpaul Exp $
 */

/*
 * Broadcom BCM570x family gigabit ethernet driver for FreeBSD.
 *
 * Written by Bill Paul <wpaul@windriver.com>
 * Senior Engineer, Wind River Systems
 */

/*
 * The Broadcom BCM5700 is based on technology originally developed by
 * Alteon Networks as part of the Tigon I and Tigon II gigabit ethernet
 * MAC chips. The BCM5700, sometimes refered to as the Tigon III, has
 * two on-board MIPS R4000 CPUs and can have as much as 16MB of external
 * SSRAM. The BCM5700 supports TCP, UDP and IP checksum offload, jumbo
 * frames, highly configurable RX filtering, and 16 RX and TX queues
 * (which, along with RX filter rules, can be used for QOS applications).
 * Other features, such as TCP segmentation, may be available as part
 * of value-added firmware updates. Unlike the Tigon I and Tigon II,
 * firmware images can be stored in hardware and need not be compiled
 * into the driver.
 *
 * The BCM5700 supports the PCI v2.2 and PCI-X v1.0 standards, and will
 * function in a 32-bit/64-bit 33/66Mhz bus, or a 64-bit/133Mhz bus.
 *
 * The BCM5701 is a single-chip solution incorporating both the BCM5700
 * MAC and a BCM5401 10/100/1000 PHY. Unlike the BCM5700, the BCM5700
 * does not support external SSRAM.
 *
 * Broadcom also produces a variation of the BCM5700 under the "Altima"
 * brand name, which is functionally similar but lacks PCI-X support.
 *
 * Without external SSRAM, you can only have at most 4 TX rings,
 * and the use of the mini RX ring is disabled. This seems to imply
 * that these features are simply not available on the BCM5701. As a
 * result, this driver does not implement any support for the mini RX
 * ring.
 */

#include "bpfilter.h"
#include "vlan.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/device.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>

#ifdef INET
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/if_ether.h>
#endif

#if NVLAN > 0
#include <net/if_types.h>
#include <net/if_vlan_var.h>
#endif

#if NBPFILTER > 0
#include <net/bpf.h>
#endif

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>
#include <dev/mii/miidevs.h>
#include <dev/mii/brgphyreg.h>

#include <dev/pci/if_bgereg.h>

/* #define BGE_CHECKSUM */

int bge_probe		__P((struct device *, void *, void *));
void bge_attach		__P((struct device *, struct device *, void *));
void bge_release_resources	__P((struct bge_softc *));
void bge_txeof		__P((struct bge_softc *));
void bge_rxeof		__P((struct bge_softc *));

void bge_tick		__P((void *));
void bge_stats_update	__P((struct bge_softc *));
int bge_encap		__P((struct bge_softc *, struct mbuf *, u_int32_t *));

int bge_intr		__P((void *));
void bge_start		__P((struct ifnet *));
int bge_ioctl		__P((struct ifnet *, u_long, caddr_t));
void bge_init		__P((void *));
void bge_stop		__P((struct bge_softc *));
void bge_watchdog	__P((struct ifnet *));
void bge_shutdown	__P((void *));
int bge_ifmedia_upd	__P((struct ifnet *));
void bge_ifmedia_sts	__P((struct ifnet *, struct ifmediareq *));

u_int8_t	bge_eeprom_getbyte	__P((struct bge_softc *,
					     int, u_int8_t *));
int bge_read_eeprom	__P((struct bge_softc *, caddr_t, int, int));

u_int32_t bge_crc	__P((struct bge_softc *, caddr_t));
void bge_setmulti	__P((struct bge_softc *));

void bge_handle_events	__P((struct bge_softc *));
int bge_alloc_jumbo_mem	__P((struct bge_softc *));
void bge_free_jumbo_mem	__P((struct bge_softc *));
void *bge_jalloc	__P((struct bge_softc *));
void bge_jfree		__P((caddr_t, u_int, void *));
int bge_newbuf_std	__P((struct bge_softc *, int, struct mbuf *));
int bge_newbuf_jumbo	__P((struct bge_softc *, int, struct mbuf *));
int bge_init_rx_ring_std	__P((struct bge_softc *));
void bge_free_rx_ring_std	__P((struct bge_softc *));
int bge_init_rx_ring_jumbo	__P((struct bge_softc *));
void bge_free_rx_ring_jumbo	__P((struct bge_softc *));
void bge_free_tx_ring	__P((struct bge_softc *));
int bge_init_tx_ring	__P((struct bge_softc *));

int bge_chipinit	__P((struct bge_softc *));
int bge_blockinit	__P((struct bge_softc *));

u_int8_t bge_vpd_readbyte	__P((struct bge_softc *, int));
void bge_vpd_read_res	__P((struct bge_softc *, struct vpd_res *, int));
void bge_vpd_read	__P((struct bge_softc *));

u_int32_t bge_readmem_ind	__P((struct bge_softc *, int));
void bge_writemem_ind	__P((struct bge_softc *, int, int));
#ifdef notdef
u_int32_t bge_readreg_ind	__P((struct bge_softc *, int));
#endif
void bge_writereg_ind	__P((struct bge_softc *, int, int));

int bge_miibus_readreg	__P((struct device *, int, int));
void bge_miibus_writereg	__P((struct device *, int, int, int));
void bge_miibus_statchg	__P((struct device *));

void bge_reset		__P((struct bge_softc *));
void bge_phy_hack	__P((struct bge_softc *));

#define BGE_DEBUG
#ifdef BGE_DEBUG
#define DPRINTF(x)	if (bgedebug) printf x
#define DPRINTFN(n,x)	if (bgedebug >= (n)) printf x
int	bgedebug = 0;
#else
#define DPRINTF(x)
#define DPRINTFN(n,x)
#endif

u_int32_t
bge_readmem_ind(sc, off)
	struct bge_softc *sc;
	int off;
{
	struct pci_attach_args	*pa = &(sc->bge_pa);

	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_MEMWIN_BASEADDR, off);
	return (pci_conf_read(pa->pa_pc, pa->pa_tag, BGE_PCI_MEMWIN_DATA));
}

void
bge_writemem_ind(sc, off, val)
	struct bge_softc *sc;
	int off, val;
{
	struct pci_attach_args	*pa = &(sc->bge_pa);

	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_MEMWIN_BASEADDR, off);
	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_MEMWIN_DATA, val);
}

#ifdef notdef
u_int32_t
bge_readreg_ind(sc, off)
	struct bge_softc *sc;
	int off;
{
	struct pci_attach_args	*pa = &(sc->bge_pa);

	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_REG_BASEADDR, off);
	return(pci_conf_read(pa->pa_pc, pa->pa_tag, BGE_PCI_REG_DATA));
}
#endif

void
bge_writereg_ind(sc, off, val)
	struct bge_softc *sc;
	int off, val;
{
	struct pci_attach_args	*pa = &(sc->bge_pa);

	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_REG_BASEADDR, off);
	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_REG_DATA, val);
}

u_int8_t
bge_vpd_readbyte(sc, addr)
	struct bge_softc *sc;
	int addr;
{
	int i;
	u_int32_t val;
	struct pci_attach_args	*pa = &(sc->bge_pa);

	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_VPD_ADDR, addr);
	for (i = 0; i < BGE_TIMEOUT * 10; i++) {
		DELAY(10);
		if (pci_conf_read(pa->pa_pc, pa->pa_tag, BGE_PCI_VPD_ADDR) &
		    BGE_VPD_FLAG)
			break;
	}

	if (i == BGE_TIMEOUT) {
		printf("%s: VPD read timed out\n", sc->bge_dev.dv_xname);
		return(0);
	}

	val = pci_conf_read(pa->pa_pc, pa->pa_tag, BGE_PCI_VPD_DATA);

	return((val >> ((addr % 4) * 8)) & 0xFF);
}

void
bge_vpd_read_res(sc, res, addr)
	struct bge_softc *sc;
	struct vpd_res *res;
	int addr;
{
	int i;
	u_int8_t *ptr;

	ptr = (u_int8_t *)res;
	for (i = 0; i < sizeof(struct vpd_res); i++)
		ptr[i] = bge_vpd_readbyte(sc, i + addr);
}

void
bge_vpd_read(sc)
	struct bge_softc *sc;
{
	int pos = 0, i;
	struct vpd_res res;

	if (sc->bge_vpd_prodname != NULL)
		free(sc->bge_vpd_prodname, M_DEVBUF);
	if (sc->bge_vpd_readonly != NULL)
		free(sc->bge_vpd_readonly, M_DEVBUF);
	sc->bge_vpd_prodname = NULL;
	sc->bge_vpd_readonly = NULL;

	bge_vpd_read_res(sc, &res, pos);

	if (res.vr_id != VPD_RES_ID) {
		printf("%s: bad VPD resource id: expected %x got %x\n",
			sc->bge_dev.dv_xname, VPD_RES_ID, res.vr_id);
		return;
	}

	pos += sizeof(res);
	sc->bge_vpd_prodname = malloc(res.vr_len + 1, M_DEVBUF, M_NOWAIT);
	for (i = 0; i < res.vr_len; i++)
		sc->bge_vpd_prodname[i] = bge_vpd_readbyte(sc, i + pos);
	sc->bge_vpd_prodname[i] = '\0';
	pos += i;

	bge_vpd_read_res(sc, &res, pos);

	if (res.vr_id != VPD_RES_READ) {
		printf("%s: bad VPD resource id: expected %x got %x\n",
		    sc->bge_dev.dv_xname, VPD_RES_READ, res.vr_id);
		return;
	}

	pos += sizeof(res);
	sc->bge_vpd_readonly = malloc(res.vr_len, M_DEVBUF, M_NOWAIT);
	for (i = 0; i < res.vr_len + 1; i++)
		sc->bge_vpd_readonly[i] = bge_vpd_readbyte(sc, i + pos);
}

/*
 * Read a byte of data stored in the EEPROM at address 'addr.' The
 * BCM570x supports both the traditional bitbang interface and an
 * auto access interface for reading the EEPROM. We use the auto
 * access method.
 */
u_int8_t
bge_eeprom_getbyte(sc, addr, dest)
	struct bge_softc *sc;
	int addr;
	u_int8_t *dest;
{
	int i;
	u_int32_t byte = 0;

	/*
	 * Enable use of auto EEPROM access so we can avoid
	 * having to use the bitbang method.
	 */
	BGE_SETBIT(sc, BGE_MISC_LOCAL_CTL, BGE_MLC_AUTO_EEPROM);

	/* Reset the EEPROM, load the clock period. */
	CSR_WRITE_4(sc, BGE_EE_ADDR,
	    BGE_EEADDR_RESET|BGE_EEHALFCLK(BGE_HALFCLK_384SCL));
	DELAY(20);

	/* Issue the read EEPROM command. */
	CSR_WRITE_4(sc, BGE_EE_ADDR, BGE_EE_READCMD | addr);

	/* Wait for completion */
	for(i = 0; i < BGE_TIMEOUT * 10; i++) {
		DELAY(10);
		if (CSR_READ_4(sc, BGE_EE_ADDR) & BGE_EEADDR_DONE)
			break;
	}

	if (i == BGE_TIMEOUT) {
		printf("%s: eeprom read timed out\n", sc->bge_dev.dv_xname);
		return(0);
	}

	/* Get result. */
	byte = CSR_READ_4(sc, BGE_EE_DATA);

	*dest = (byte >> ((addr % 4) * 8)) & 0xFF;

	return(0);
}

/*
 * Read a sequence of bytes from the EEPROM.
 */
int
bge_read_eeprom(sc, dest, off, cnt)
	struct bge_softc *sc;
	caddr_t dest;
	int off;
	int cnt;
{
	int err = 0, i;
	u_int8_t byte = 0;

	for (i = 0; i < cnt; i++) {
		err = bge_eeprom_getbyte(sc, off + i, &byte);
		if (err)
			break;
		*(dest + i) = byte;
	}

	return(err ? 1 : 0);
}

int
bge_miibus_readreg(dev, phy, reg)
	struct device *dev;
	int phy, reg;
{
	struct bge_softc *sc = (struct bge_softc *)dev;
	struct ifnet *ifp;
	u_int32_t val;
	int i;

	ifp = &sc->arpcom.ac_if;

	if (ifp->if_flags & IFF_RUNNING)
		BGE_CLRBIT(sc, BGE_MI_MODE, BGE_MIMODE_AUTOPOLL);

	CSR_WRITE_4(sc, BGE_MI_COMM, BGE_MICMD_READ|BGE_MICOMM_BUSY|
	    BGE_MIPHY(phy)|BGE_MIREG(reg));

	for (i = 0; i < BGE_TIMEOUT; i++) {
		val = CSR_READ_4(sc, BGE_MI_COMM);
		if (!(val & BGE_MICOMM_BUSY))
			break;
	}

	if (i == BGE_TIMEOUT) {
		printf("%s: PHY read timed out\n", sc->bge_dev.dv_xname);
		return(0);
	}

	val = CSR_READ_4(sc, BGE_MI_COMM);

	if (ifp->if_flags & IFF_RUNNING)
		BGE_SETBIT(sc, BGE_MI_MODE, BGE_MIMODE_AUTOPOLL);

	if (val & BGE_MICOMM_READFAIL)
		return(0);

	return(val & 0xFFFF);
}

void
bge_miibus_writereg(dev, phy, reg, val)
	struct device *dev;
	int phy, reg, val;
{
	struct bge_softc *sc = (struct bge_softc *)dev;
	int i;

	CSR_WRITE_4(sc, BGE_MI_COMM, BGE_MICMD_WRITE|BGE_MICOMM_BUSY|
	    BGE_MIPHY(phy)|BGE_MIREG(reg)|val);

	for (i = 0; i < BGE_TIMEOUT; i++) {
		if (!(CSR_READ_4(sc, BGE_MI_COMM) & BGE_MICOMM_BUSY))
			break;
	}

	if (i == BGE_TIMEOUT) {
		printf("%s: PHY read timed out\n", sc->bge_dev.dv_xname);
	}
}

void
bge_miibus_statchg(dev)
	struct device *dev;
{
	struct bge_softc *sc = (struct bge_softc *)dev;
	struct mii_data *mii = &sc->bge_mii;

	BGE_CLRBIT(sc, BGE_MAC_MODE, BGE_MACMODE_PORTMODE);
	if (IFM_SUBTYPE(mii->mii_media_active) == IFM_1000_TX) {
		BGE_SETBIT(sc, BGE_MAC_MODE, BGE_PORTMODE_GMII);
	} else {
		BGE_SETBIT(sc, BGE_MAC_MODE, BGE_PORTMODE_MII);
	}

	if ((mii->mii_media_active & IFM_GMASK) == IFM_FDX) {
		BGE_CLRBIT(sc, BGE_MAC_MODE, BGE_MACMODE_HALF_DUPLEX);
	} else {
		BGE_SETBIT(sc, BGE_MAC_MODE, BGE_MACMODE_HALF_DUPLEX);
	}

	bge_phy_hack(sc);
}

/*
 * Handle events that have triggered interrupts.
 */
void
bge_handle_events(sc)
	struct bge_softc		*sc;
{

	return;
}

/*
 * Memory management for jumbo frames.
 */

int
bge_alloc_jumbo_mem(sc)
	struct bge_softc		*sc;
{
	caddr_t			ptr, kva;
	bus_dma_segment_t	seg;
	int		i, rseg;
	struct bge_jpool_entry   *entry;

	/* Grab a big chunk o' storage. */
	if (bus_dmamem_alloc(sc->bge_dmatag, BGE_JMEM, PAGE_SIZE, 0,
			     &seg, 1, &rseg, BUS_DMA_NOWAIT)) {
		printf("%s: can't alloc rx buffers\n", sc->bge_dev.dv_xname);
		return (ENOBUFS);
	}
	if (bus_dmamem_map(sc->bge_dmatag, &seg, rseg, BGE_JMEM, &kva,
			   BUS_DMA_NOWAIT)) {
		printf("%s: can't map dma buffers (%d bytes)\n",
		    sc->bge_dev.dv_xname, BGE_JMEM);
		bus_dmamem_free(sc->bge_dmatag, &seg, rseg);
		return (ENOBUFS);
	}
	if (bus_dmamap_create(sc->bge_dmatag, BGE_JMEM, 1, BGE_JMEM, 0,
	    BUS_DMA_NOWAIT, &sc->bge_cdata.bge_rx_jumbo_map)) {
		printf("%s: can't create dma map\n", sc->bge_dev.dv_xname);
		bus_dmamem_unmap(sc->bge_dmatag, kva, BGE_JMEM);
		bus_dmamem_free(sc->bge_dmatag, &seg, rseg);
		return (ENOBUFS);
	}
	if (bus_dmamap_load(sc->bge_dmatag, sc->bge_cdata.bge_rx_jumbo_map,
			    kva, BGE_JMEM, NULL, BUS_DMA_NOWAIT)) {
		printf("%s: can't load dma map\n", sc->bge_dev.dv_xname);
		bus_dmamap_destroy(sc->bge_dmatag,
				   sc->bge_cdata.bge_rx_jumbo_map);
		bus_dmamem_unmap(sc->bge_dmatag, kva, BGE_JMEM);
		bus_dmamem_free(sc->bge_dmatag, &seg, rseg);
		return (ENOBUFS);
	}
	sc->bge_cdata.bge_jumbo_buf = (caddr_t)kva;
	DPRINTFN(1,("bge_jumbo_buf = 0x%08X\n", sc->bge_cdata.bge_jumbo_buf));

	LIST_INIT(&sc->bge_jfree_listhead);
	LIST_INIT(&sc->bge_jinuse_listhead);

	/*
	 * Now divide it up into 9K pieces and save the addresses
	 * in an array.
	 */
	ptr = sc->bge_cdata.bge_jumbo_buf;
	for (i = 0; i < BGE_JSLOTS; i++) {
		sc->bge_cdata.bge_jslots[i] = ptr;
		ptr += BGE_JLEN;
		entry = malloc(sizeof(struct bge_jpool_entry),
		    M_DEVBUF, M_NOWAIT);
		if (entry == NULL) {
			bus_dmamap_unload(sc->bge_dmatag,
					  sc->bge_cdata.bge_rx_jumbo_map);
			bus_dmamap_destroy(sc->bge_dmatag,
					   sc->bge_cdata.bge_rx_jumbo_map);
			bus_dmamem_unmap(sc->bge_dmatag, kva, BGE_JMEM);
			bus_dmamem_free(sc->bge_dmatag, &seg, rseg);
			sc->bge_cdata.bge_jumbo_buf = NULL;
			printf("%s: no memory for jumbo buffer queue!\n",
			    sc->bge_dev.dv_xname);
			return(ENOBUFS);
		}
		entry->slot = i;
		LIST_INSERT_HEAD(&sc->bge_jfree_listhead,
				 entry, jpool_entries);
	}

	return(0);
}

/*
 * Allocate a jumbo buffer.
 */
void *
bge_jalloc(sc)
	struct bge_softc		*sc;
{
	struct bge_jpool_entry   *entry;

	entry = LIST_FIRST(&sc->bge_jfree_listhead);

	if (entry == NULL) {
		printf("%s: no free jumbo buffers\n", sc->bge_dev.dv_xname);
		return(NULL);
	}

	LIST_REMOVE(entry, jpool_entries);
	LIST_INSERT_HEAD(&sc->bge_jinuse_listhead, entry, jpool_entries);
	return(sc->bge_cdata.bge_jslots[entry->slot]);
}

/*
 * Release a jumbo buffer.
 */
void
bge_jfree(buf, size, arg)
	caddr_t		buf;
	u_int		size;
	void		*arg;
{
	struct bge_jpool_entry *entry;
	struct bge_softc *sc;
	int i;

	/* Extract the softc struct pointer. */
	sc = (struct bge_softc *)arg;

	if (sc == NULL)
		panic("bge_jfree: can't find softc pointer!");

	/* calculate the slot this buffer belongs to */

	i = ((vm_offset_t)buf
	     - (vm_offset_t)sc->bge_cdata.bge_jumbo_buf) / BGE_JLEN;

	if ((i < 0) || (i >= BGE_JSLOTS))
		panic("bge_jfree: asked to free buffer that we don't manage!");

	entry = LIST_FIRST(&sc->bge_jinuse_listhead);
	if (entry == NULL)
		panic("bge_jfree: buffer not in use!");
	entry->slot = i;
	LIST_REMOVE(entry, jpool_entries);
	LIST_INSERT_HEAD(&sc->bge_jfree_listhead, entry, jpool_entries);
}


/*
 * Intialize a standard receive ring descriptor.
 */
int
bge_newbuf_std(sc, i, m)
	struct bge_softc	*sc;
	int			i;
	struct mbuf		*m;
{
	struct mbuf		*m_new = NULL;
	struct bge_rx_bd	*r;
	bus_dmamap_t		rxmap = sc->bge_cdata.bge_rx_std_map[i];

	if (m == NULL) {
		MGETHDR(m_new, M_DONTWAIT, MT_DATA);
		if (m_new == NULL) {
			printf("%s: mbuf allocation failed "
			    "-- packet dropped!\n", sc->bge_dev.dv_xname);
			return(ENOBUFS);
		}

		MCLGET(m_new, M_DONTWAIT);
		if (!(m_new->m_flags & M_EXT)) {
			printf("%s: cluster allocation failed "
			    "-- packet dropped!\n", sc->bge_dev.dv_xname);
			m_freem(m_new);
			return(ENOBUFS);
		}
		m_new->m_len = m_new->m_pkthdr.len = MCLBYTES;
	} else {
		m_new = m;
		m_new->m_len = m_new->m_pkthdr.len = MCLBYTES;
		m_new->m_data = m_new->m_ext.ext_buf;
	}

	if (bus_dmamap_load_mbuf(sc->bge_dmatag, rxmap, m_new, BUS_DMA_NOWAIT))
		return(ENOBUFS);

	m_adj(m_new, ETHER_ALIGN);
	sc->bge_cdata.bge_rx_std_chain[i] = m_new;
	r = &sc->bge_rdata->bge_rx_std_ring[i];
	BGE_HOSTADDR(r->bge_addr) = rxmap->dm_segs[0].ds_addr + ETHER_ALIGN;
	r->bge_flags = BGE_RXBDFLAG_END;
	r->bge_len = m_new->m_len;
	r->bge_idx = i;

	return(0);
}

/*
 * Initialize a jumbo receive ring descriptor. This allocates
 * a jumbo buffer from the pool managed internally by the driver.
 */
int
bge_newbuf_jumbo(sc, i, m)
	struct bge_softc *sc;
	int i;
	struct mbuf *m;
{
	struct mbuf *m_new = NULL;
	struct bge_rx_bd *r;

	if (m == NULL) {
		caddr_t			*buf = NULL;

		/* Allocate the mbuf. */
		MGETHDR(m_new, M_DONTWAIT, MT_DATA);
		if (m_new == NULL) {
			printf("%s: mbuf allocation failed "
			    "-- packet dropped!\n", sc->bge_dev.dv_xname);
			return(ENOBUFS);
		}

		/* Allocate the jumbo buffer */
		buf = bge_jalloc(sc);
		if (buf == NULL) {
			m_freem(m_new);
			printf("%s: jumbo allocation failed "
			    "-- packet dropped!\n", sc->bge_dev.dv_xname);
			return(ENOBUFS);
		}

		/* Attach the buffer to the mbuf. */
		m_new->m_len = m_new->m_pkthdr.len = BGE_JUMBO_FRAMELEN;
		MEXTADD(m_new, buf, BGE_JUMBO_FRAMELEN, 0, bge_jfree, sc);
	} else {
		m_new = m;
		m_new->m_data = m_new->m_ext.ext_buf;
		m_new->m_ext.ext_size = BGE_JUMBO_FRAMELEN;
	}

	m_adj(m_new, ETHER_ALIGN);
	/* Set up the descriptor. */
	r = &sc->bge_rdata->bge_rx_jumbo_ring[i];
	sc->bge_cdata.bge_rx_jumbo_chain[i] = m_new;
	BGE_HOSTADDR(r->bge_addr) =
		BGE_JUMBO_DMA_ADDR(sc, m_new) + ETHER_ALIGN;
	r->bge_flags = BGE_RXBDFLAG_END|BGE_RXBDFLAG_JUMBO_RING;
	r->bge_len = m_new->m_len;
	r->bge_idx = i;

	return(0);
}

/*
 * The standard receive ring has 512 entries in it. At 2K per mbuf cluster,
 * that's 1MB or memory, which is a lot. For now, we fill only the first
 * 256 ring entries and hope that our CPU is fast enough to keep up with
 * the NIC.
 */
int
bge_init_rx_ring_std(sc)
	struct bge_softc *sc;
{
	int i;

	for (i = 0; i < BGE_STD_RX_RING_CNT; i++) {
		if (bus_dmamap_create(sc->bge_dmatag, MCLBYTES, 1, MCLBYTES,
		    0, BUS_DMA_NOWAIT, &sc->bge_cdata.bge_rx_std_map[i]))
			return(ENOBUFS);
	}

	for (i = 0; i < BGE_SSLOTS; i++) {
		if (bge_newbuf_std(sc, i, NULL) == ENOBUFS)
			return(ENOBUFS);
	}

	sc->bge_std = i - 1;
	CSR_WRITE_4(sc, BGE_MBX_RX_STD_PROD_LO, sc->bge_std);

	return(0);
}

void
bge_free_rx_ring_std(sc)
	struct bge_softc *sc;
{
	int i;

	for (i = 0; i < BGE_STD_RX_RING_CNT; i++) {
		if (sc->bge_cdata.bge_rx_std_chain[i] != NULL) {
			m_freem(sc->bge_cdata.bge_rx_std_chain[i]);
			sc->bge_cdata.bge_rx_std_chain[i] = NULL;
			bus_dmamap_unload(sc->bge_dmatag,
					  sc->bge_cdata.bge_rx_std_map[i]);
		}
		bzero((char *)&sc->bge_rdata->bge_rx_std_ring[i],
		    sizeof(struct bge_rx_bd));
	}
}

int
bge_init_rx_ring_jumbo(sc)
	struct bge_softc *sc;
{
	int i;
	struct bge_rcb *rcb;
	struct bge_rcb_opaque *rcbo;

	for (i = 0; i < BGE_JUMBO_RX_RING_CNT; i++) {
		if (bge_newbuf_jumbo(sc, i, NULL) == ENOBUFS)
			return(ENOBUFS);
	};

	sc->bge_jumbo = i - 1;

	rcb = &sc->bge_rdata->bge_info.bge_jumbo_rx_rcb;
	rcbo = (struct bge_rcb_opaque *)rcb;
	rcb->bge_flags = 0;
	CSR_WRITE_4(sc, BGE_RX_JUMBO_RCB_MAXLEN_FLAGS, rcbo->bge_reg2);

	CSR_WRITE_4(sc, BGE_MBX_RX_JUMBO_PROD_LO, sc->bge_jumbo);

	return(0);
}

void
bge_free_rx_ring_jumbo(sc)
	struct bge_softc *sc;
{
	int i;

	for (i = 0; i < BGE_JUMBO_RX_RING_CNT; i++) {
		if (sc->bge_cdata.bge_rx_jumbo_chain[i] != NULL) {
			m_freem(sc->bge_cdata.bge_rx_jumbo_chain[i]);
			sc->bge_cdata.bge_rx_jumbo_chain[i] = NULL;
		}
		bzero((char *)&sc->bge_rdata->bge_rx_jumbo_ring[i],
		    sizeof(struct bge_rx_bd));
	}
}

void
bge_free_tx_ring(sc)
	struct bge_softc *sc;
{
	int i;

	if (sc->bge_rdata->bge_tx_ring == NULL)
		return;

	for (i = 0; i < BGE_TX_RING_CNT; i++) {
		if (sc->bge_cdata.bge_tx_chain[i] != NULL) {
			m_freem(sc->bge_cdata.bge_tx_chain[i]);
			sc->bge_cdata.bge_tx_chain[i] = NULL;
			bus_dmamap_unload(sc->bge_dmatag,
					  sc->bge_cdata.bge_tx_map[i]);
		}
		bzero((char *)&sc->bge_rdata->bge_tx_ring[i],
		    sizeof(struct bge_tx_bd));
	}
}

int
bge_init_tx_ring(sc)
	struct bge_softc *sc;
{
	int i;

	sc->bge_txcnt = 0;
	sc->bge_tx_saved_considx = 0;
	CSR_WRITE_4(sc, BGE_MBX_TX_HOST_PROD0_LO, 0);
	CSR_WRITE_4(sc, BGE_MBX_TX_NIC_PROD0_LO, 0);

	for (i = 0; i < BGE_TX_RING_CNT; i++) {
		if (bus_dmamap_create(sc->bge_dmatag, MCLBYTES, BGE_NTXSEG,
		    MCLBYTES, 0, BUS_DMA_NOWAIT, &sc->bge_cdata.bge_tx_map[i]))
			return(ENOBUFS);
	}

	return(0);
}

#define BGE_POLY	0xEDB88320

u_int32_t
bge_crc(sc, addr)
	struct bge_softc *sc;
	caddr_t addr;
{
	u_int32_t idx, bit, data, crc;

	/* Compute CRC for the address value. */
	crc = 0xFFFFFFFF; /* initial value */

	for (idx = 0; idx < 6; idx++) {
		for (data = *addr++, bit = 0; bit < 8; bit++, data >>= 1)
			crc = (crc >> 1) ^ (((crc ^ data) & 1) ? BGE_POLY : 0);
	}

	return(crc & 0x7F);
}

void
bge_setmulti(sc)
	struct bge_softc *sc;
{
	struct arpcom		*ac = &sc->arpcom;
	struct ifnet		*ifp = &ac->ac_if;
	struct ether_multi	*enm;
	struct ether_multistep  step;
	u_int32_t		hashes[4] = { 0, 0, 0, 0 };
	u_int32_t		h;
	int			i;

	if (ifp->if_flags & IFF_ALLMULTI || ifp->if_flags & IFF_PROMISC) {
		for (i = 0; i < 4; i++)
			CSR_WRITE_4(sc, BGE_MAR0 + (i * 4), 0xFFFFFFFF);
		return;
	}

	/* First, zot all the existing filters. */
	for (i = 0; i < 4; i++)
		CSR_WRITE_4(sc, BGE_MAR0 + (i * 4), 0);

	/* Now program new ones. */
	ETHER_FIRST_MULTI(step, ac, enm);
	while (enm != NULL) {
		h = bge_crc(sc, LLADDR((struct sockaddr_dl *)enm->enm_addrlo));
		hashes[(h & 0x60) >> 5] |= 1 << (h & 0x1F);
		ETHER_NEXT_MULTI(step, enm);
	}

	for (i = 0; i < 4; i++)
		CSR_WRITE_4(sc, BGE_MAR0 + (i * 4), hashes[i]);
}

/*
 * Do endian, PCI and DMA initialization. Also check the on-board ROM
 * self-test results.
 */
int
bge_chipinit(sc)
	struct bge_softc *sc;
{
	u_int32_t		cachesize;
	int			i;
	struct pci_attach_args	*pa = &(sc->bge_pa);

#ifdef BGE_CHECKSUM
	sc->arpcom.ac_if.if_capabilities =
	  IFCAP_CSUM_IPv4 | IFCAP_CSUM_TCPv4 | IFCAP_CSUM_UDPv4;
#endif

	/* Set endianness before we access any non-PCI registers. */
#if BYTE_ORDER == BIG_ENDIAN
	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_MISC_CTL,
	    BGE_BIGENDIAN_INIT);
#else
	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_MISC_CTL,
	    BGE_LITTLEENDIAN_INIT);
#endif

	/*
	 * Check the 'ROM failed' bit on the RX CPU to see if
	 * self-tests passed.
	 */
	if (CSR_READ_4(sc, BGE_RXCPU_MODE) & BGE_RXCPUMODE_ROMFAIL) {
		printf("%s: RX CPU self-diagnostics failed!\n",
		    sc->bge_dev.dv_xname);
		return(ENODEV);
	}

	/* Clear the MAC control register */
	CSR_WRITE_4(sc, BGE_MAC_MODE, 0);

	/*
	 * Clear the MAC statistics block in the NIC's
	 * internal memory.
	 */
	for (i = BGE_STATS_BLOCK;
	    i < BGE_STATS_BLOCK_END + 1; i += sizeof(u_int32_t))
		BGE_MEMWIN_WRITE(pa->pa_pc, pa->pa_tag, i, 0);

	for (i = BGE_STATUS_BLOCK;
	    i < BGE_STATUS_BLOCK_END + 1; i += sizeof(u_int32_t))
		BGE_MEMWIN_WRITE(pa->pa_pc, pa->pa_tag, i, 0);

	/* Set up the PCI DMA control register. */
	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_DMA_RW_CTL,
	    BGE_PCI_READ_CMD|BGE_PCI_WRITE_CMD|0x0F);

	/*
	 * Set up general mode register.
	 */
#ifndef BGE_CHECKSUM
	CSR_WRITE_4(sc, BGE_MODE_CTL, BGE_MODECTL_WORDSWAP_NONFRAME|
		    BGE_MODECTL_BYTESWAP_DATA|BGE_MODECTL_WORDSWAP_DATA|
		    BGE_MODECTL_MAC_ATTN_INTR|BGE_MODECTL_HOST_SEND_BDS|
		    BGE_MODECTL_NO_RX_CRC);
#else
	CSR_WRITE_4(sc, BGE_MODE_CTL, BGE_MODECTL_WORDSWAP_NONFRAME|
		    BGE_MODECTL_BYTESWAP_DATA|BGE_MODECTL_WORDSWAP_DATA|
		    BGE_MODECTL_MAC_ATTN_INTR|BGE_MODECTL_HOST_SEND_BDS|
		    BGE_MODECTL_NO_RX_CRC
/*		    |BGE_MODECTL_TX_NO_PHDR_CSUM| */
/*		    BGE_MODECTL_RX_NO_PHDR_CSUM */
);
#endif

	/* Get cache line size. */
	cachesize = pci_conf_read(pa->pa_pc, pa->pa_tag, BGE_PCI_CACHESZ);

	/*
	 * Avoid violating PCI spec on certain chip revs.
	 */
	if (pci_conf_read(pa->pa_pc, pa->pa_tag, BGE_PCI_CMD) &
	    PCIM_CMD_MWIEN) {
		switch(cachesize) {
		case 1:
			PCI_SETBIT(pa->pa_pc, pa->pa_tag, BGE_PCI_DMA_RW_CTL,
				   BGE_PCI_WRITE_BNDRY_16BYTES);
			break;
		case 2:
			PCI_SETBIT(pa->pa_pc, pa->pa_tag, BGE_PCI_DMA_RW_CTL,
				   BGE_PCI_WRITE_BNDRY_32BYTES);
			break;
		case 4:
			PCI_SETBIT(pa->pa_pc, pa->pa_tag, BGE_PCI_DMA_RW_CTL,
				   BGE_PCI_WRITE_BNDRY_64BYTES);
			break;
		case 8:
			PCI_SETBIT(pa->pa_pc, pa->pa_tag, BGE_PCI_DMA_RW_CTL,
				   BGE_PCI_WRITE_BNDRY_128BYTES);
			break;
		case 16:
			PCI_SETBIT(pa->pa_pc, pa->pa_tag, BGE_PCI_DMA_RW_CTL,
				   BGE_PCI_WRITE_BNDRY_256BYTES);
			break;
		case 32:
			PCI_SETBIT(pa->pa_pc, pa->pa_tag, BGE_PCI_DMA_RW_CTL,
				   BGE_PCI_WRITE_BNDRY_512BYTES);
			break;
		case 64:
			PCI_SETBIT(pa->pa_pc, pa->pa_tag, BGE_PCI_DMA_RW_CTL,
				   BGE_PCI_WRITE_BNDRY_1024BYTES);
			break;
		default:
		/* Disable PCI memory write and invalidate. */
#if 0
			if (bootverbose)
				printf("%s: cache line size %d not "
				    "supported; disabling PCI MWI\n",
				    sc->bge_dev.dv_xname, cachesize);
#endif
			PCI_CLRBIT(pa->pa_pc, pa->pa_tag, BGE_PCI_CMD,
			    PCIM_CMD_MWIEN);
			break;
		}
	}

#ifdef __brokenalpha__
	/*
	 * Must insure that we do not cross an 8K (bytes) boundary
	 * for DMA reads.  Our highest limit is 1K bytes.  This is a
	 * restriction on some ALPHA platforms with early revision
	 * 21174 PCI chipsets, such as the AlphaPC 164lx
	 */
	PCI_SETBIT(sc, BGE_PCI_DMA_RW_CTL, BGE_PCI_READ_BNDRY_1024, 4);
#endif

	/* Set the timer prescaler (always 66Mhz) */
	CSR_WRITE_4(sc, BGE_MISC_CFG, 65 << 1/*BGE_32BITTIME_66MHZ*/);

	return(0);
}

int
bge_blockinit(sc)
	struct bge_softc *sc;
{
	struct bge_rcb		*rcb;
	struct bge_rcb_opaque	*rcbo;
	vm_offset_t		rcb_addr;
	int			i;

	/*
	 * Initialize the memory window pointer register so that
	 * we can access the first 32K of internal NIC RAM. This will
	 * allow us to set up the TX send ring RCBs and the RX return
	 * ring RCBs, plus other things which live in NIC memory.
	 */
	CSR_WRITE_4(sc, BGE_PCI_MEMWIN_BASEADDR, 0);

	/* Configure mbuf memory pool */
	if (sc->bge_extram) {
		CSR_WRITE_4(sc, BGE_BMAN_MBUFPOOL_BASEADDR, BGE_EXT_SSRAM);
		CSR_WRITE_4(sc, BGE_BMAN_MBUFPOOL_LEN, 0x18000);
	} else {
		CSR_WRITE_4(sc, BGE_BMAN_MBUFPOOL_BASEADDR, BGE_BUFFPOOL_1);
		CSR_WRITE_4(sc, BGE_BMAN_MBUFPOOL_LEN, 0x18000);
	}

	/* Configure DMA resource pool */
	CSR_WRITE_4(sc, BGE_BMAN_DMA_DESCPOOL_BASEADDR, BGE_DMA_DESCRIPTORS);
	CSR_WRITE_4(sc, BGE_BMAN_DMA_DESCPOOL_LEN, 0x2000);

	/* Configure mbuf pool watermarks */
	CSR_WRITE_4(sc, BGE_BMAN_MBUFPOOL_READDMA_LOWAT, 24);
	CSR_WRITE_4(sc, BGE_BMAN_MBUFPOOL_MACRX_LOWAT, 24);
	CSR_WRITE_4(sc, BGE_BMAN_MBUFPOOL_HIWAT, 48);

	/* Configure DMA resource watermarks */
	CSR_WRITE_4(sc, BGE_BMAN_DMA_DESCPOOL_LOWAT, 5);
	CSR_WRITE_4(sc, BGE_BMAN_DMA_DESCPOOL_HIWAT, 10);

	/* Enable buffer manager */
	CSR_WRITE_4(sc, BGE_BMAN_MODE,
	    BGE_BMANMODE_ENABLE|BGE_BMANMODE_LOMBUF_ATTN);

	/* Poll for buffer manager start indication */
	for (i = 0; i < BGE_TIMEOUT; i++) {
		if (CSR_READ_4(sc, BGE_BMAN_MODE) & BGE_BMANMODE_ENABLE)
			break;
		DELAY(10);
	}

	if (i == BGE_TIMEOUT) {
		printf("%s: buffer manager failed to start\n",
		    sc->bge_dev.dv_xname);
		return(ENXIO);
	}

	/* Enable flow-through queues */
	CSR_WRITE_4(sc, BGE_FTQ_RESET, 0xFFFFFFFF);
	CSR_WRITE_4(sc, BGE_FTQ_RESET, 0);

	/* Wait until queue initialization is complete */
	for (i = 0; i < BGE_TIMEOUT; i++) {
		if (CSR_READ_4(sc, BGE_FTQ_RESET) == 0)
			break;
		DELAY(10);
	}

	if (i == BGE_TIMEOUT) {
		printf("%s: flow-through queue init failed\n",
		    sc->bge_dev.dv_xname);
		return(ENXIO);
	}

	/* Initialize the standard RX ring control block */
	rcb = &sc->bge_rdata->bge_info.bge_std_rx_rcb;
	BGE_HOSTADDR(rcb->bge_hostaddr) =
		BGE_RING_DMA_ADDR(sc, bge_rx_std_ring);
	rcb->bge_max_len = BGE_MAX_FRAMELEN;
	if (sc->bge_extram)
		rcb->bge_nicaddr = BGE_EXT_STD_RX_RINGS;
	else
		rcb->bge_nicaddr = BGE_STD_RX_RINGS;
	rcb->bge_flags = 0;
	rcbo = (struct bge_rcb_opaque *)rcb;
	CSR_WRITE_4(sc, BGE_RX_STD_RCB_HADDR_HI, rcbo->bge_reg0);
	CSR_WRITE_4(sc, BGE_RX_STD_RCB_HADDR_LO, rcbo->bge_reg1);
	CSR_WRITE_4(sc, BGE_RX_STD_RCB_MAXLEN_FLAGS, rcbo->bge_reg2);
	CSR_WRITE_4(sc, BGE_RX_STD_RCB_NICADDR, rcbo->bge_reg3);

	/*
	 * Initialize the jumbo RX ring control block
	 * We set the 'ring disabled' bit in the flags
	 * field until we're actually ready to start
	 * using this ring (i.e. once we set the MTU
	 * high enough to require it).
	 */
	rcb = &sc->bge_rdata->bge_info.bge_jumbo_rx_rcb;
	BGE_HOSTADDR(rcb->bge_hostaddr) =
		BGE_RING_DMA_ADDR(sc, bge_rx_jumbo_ring);
	rcb->bge_max_len = BGE_MAX_FRAMELEN;
	if (sc->bge_extram)
		rcb->bge_nicaddr = BGE_EXT_JUMBO_RX_RINGS;
	else
		rcb->bge_nicaddr = BGE_JUMBO_RX_RINGS;
	rcb->bge_flags = BGE_RCB_FLAG_RING_DISABLED;

	rcbo = (struct bge_rcb_opaque *)rcb;
	CSR_WRITE_4(sc, BGE_RX_JUMBO_RCB_HADDR_HI, rcbo->bge_reg0);
	CSR_WRITE_4(sc, BGE_RX_JUMBO_RCB_HADDR_LO, rcbo->bge_reg1);
	CSR_WRITE_4(sc, BGE_RX_JUMBO_RCB_MAXLEN_FLAGS, rcbo->bge_reg2);
	CSR_WRITE_4(sc, BGE_RX_JUMBO_RCB_NICADDR, rcbo->bge_reg3);

	/* Set up dummy disabled mini ring RCB */
	rcb = &sc->bge_rdata->bge_info.bge_mini_rx_rcb;
	rcb->bge_flags = BGE_RCB_FLAG_RING_DISABLED;
	rcbo = (struct bge_rcb_opaque *)rcb;
	CSR_WRITE_4(sc, BGE_RX_MINI_RCB_MAXLEN_FLAGS, rcbo->bge_reg2);

	/*
	 * Set the BD ring replentish thresholds. The recommended
	 * values are 1/8th the number of descriptors allocated to
	 * each ring.
	 */
	CSR_WRITE_4(sc, BGE_RBDI_STD_REPL_THRESH, BGE_STD_RX_RING_CNT/8);
	CSR_WRITE_4(sc, BGE_RBDI_JUMBO_REPL_THRESH, BGE_JUMBO_RX_RING_CNT/8);

	/*
	 * Disable all unused send rings by setting the 'ring disabled'
	 * bit in the flags field of all the TX send ring control blocks.
	 * These are located in NIC memory.
	 */
	rcb_addr = BGE_MEMWIN_START + BGE_SEND_RING_RCB;
	for (i = 0; i < BGE_TX_RINGS_EXTSSRAM_MAX; i++) {
		RCB_WRITE_2(sc, rcb_addr, bge_flags,
			    BGE_RCB_FLAG_RING_DISABLED);
		RCB_WRITE_2(sc, rcb_addr, bge_max_len, 0);
		RCB_WRITE_4(sc, rcb_addr, bge_nicaddr, 0);
		rcb_addr += sizeof(struct bge_rcb);
	}

	/* Configure TX RCB 0 (we use only the first ring) */
	rcb_addr = BGE_MEMWIN_START + BGE_SEND_RING_RCB;
	RCB_WRITE_4(sc, rcb_addr, bge_hostaddr.bge_addr_hi, 0);
	RCB_WRITE_4(sc, rcb_addr, BGE_HOSTADDR(bge_hostaddr),
		    BGE_RING_DMA_ADDR(sc, bge_tx_ring));
	RCB_WRITE_4(sc, rcb_addr, bge_nicaddr,
		    BGE_NIC_TXRING_ADDR(0, BGE_TX_RING_CNT));
	RCB_WRITE_2(sc, rcb_addr, bge_max_len, BGE_TX_RING_CNT);
	RCB_WRITE_2(sc, rcb_addr, bge_flags, 0);

	/* Disable all unused RX return rings */
	rcb_addr = BGE_MEMWIN_START + BGE_RX_RETURN_RING_RCB;
	for (i = 0; i < BGE_RX_RINGS_MAX; i++) {
		RCB_WRITE_4(sc, rcb_addr, bge_hostaddr.bge_addr_hi, 0);
		RCB_WRITE_4(sc, rcb_addr, bge_hostaddr.bge_addr_lo, 0);
		RCB_WRITE_2(sc, rcb_addr, bge_flags,
			    BGE_RCB_FLAG_RING_DISABLED);
		RCB_WRITE_2(sc, rcb_addr, bge_max_len, BGE_RETURN_RING_CNT);
		RCB_WRITE_4(sc, rcb_addr, bge_nicaddr, 0);
		CSR_WRITE_4(sc, BGE_MBX_RX_CONS0_LO +
		    (i * (sizeof(u_int64_t))), 0);
		rcb_addr += sizeof(struct bge_rcb);
	}

	/* Initialize RX ring indexes */
	CSR_WRITE_4(sc, BGE_MBX_RX_STD_PROD_LO, 0);
	CSR_WRITE_4(sc, BGE_MBX_RX_JUMBO_PROD_LO, 0);
	CSR_WRITE_4(sc, BGE_MBX_RX_MINI_PROD_LO, 0);

	/*
	 * Set up RX return ring 0
	 * Note that the NIC address for RX return rings is 0x00000000.
	 * The return rings live entirely within the host, so the
	 * nicaddr field in the RCB isn't used.
	 */
	rcb_addr = BGE_MEMWIN_START + BGE_RX_RETURN_RING_RCB;
	RCB_WRITE_4(sc, rcb_addr, bge_hostaddr.bge_addr_hi, 0);
	RCB_WRITE_4(sc, rcb_addr, BGE_HOSTADDR(bge_hostaddr),
		    BGE_RING_DMA_ADDR(sc, bge_rx_return_ring));
	RCB_WRITE_4(sc, rcb_addr, bge_nicaddr, 0x00000000);
	RCB_WRITE_2(sc, rcb_addr, bge_max_len, BGE_RETURN_RING_CNT);
	RCB_WRITE_2(sc, rcb_addr, bge_flags, 0);

	/* Set random backoff seed for TX */
	CSR_WRITE_4(sc, BGE_TX_RANDOM_BACKOFF,
	    sc->arpcom.ac_enaddr[0] + sc->arpcom.ac_enaddr[1] +
	    sc->arpcom.ac_enaddr[2] + sc->arpcom.ac_enaddr[3] +
	    sc->arpcom.ac_enaddr[4] + sc->arpcom.ac_enaddr[5] +
	    BGE_TX_BACKOFF_SEED_MASK);

	/* Set inter-packet gap */
	CSR_WRITE_4(sc, BGE_TX_LENGTHS, 0x2620);

	/*
	 * Specify which ring to use for packets that don't match
	 * any RX rules.
	 */
	CSR_WRITE_4(sc, BGE_RX_RULES_CFG, 0x08);

	/*
	 * Configure number of RX lists. One interrupt distribution
	 * list, sixteen active lists, one bad frames class.
	 */
	CSR_WRITE_4(sc, BGE_RXLP_CFG, 0x181);

	/* Inialize RX list placement stats mask. */
	CSR_WRITE_4(sc, BGE_RXLP_STATS_ENABLE_MASK, 0x007FFFFF);
	CSR_WRITE_4(sc, BGE_RXLP_STATS_CTL, 0x1);

	/* Disable host coalescing until we get it set up */
	CSR_WRITE_4(sc, BGE_HCC_MODE, 0x00000000);

	/* Poll to make sure it's shut down. */
	for (i = 0; i < BGE_TIMEOUT; i++) {
		if (!(CSR_READ_4(sc, BGE_HCC_MODE) & BGE_HCCMODE_ENABLE))
			break;
		DELAY(10);
	}

	if (i == BGE_TIMEOUT) {
		printf("%s: host coalescing engine failed to idle\n",
		    sc->bge_dev.dv_xname);
		return(ENXIO);
	}

	/* Set up host coalescing defaults */
	CSR_WRITE_4(sc, BGE_HCC_RX_COAL_TICKS, sc->bge_rx_coal_ticks);
	CSR_WRITE_4(sc, BGE_HCC_TX_COAL_TICKS, sc->bge_tx_coal_ticks);
	CSR_WRITE_4(sc, BGE_HCC_RX_MAX_COAL_BDS, sc->bge_rx_max_coal_bds);
	CSR_WRITE_4(sc, BGE_HCC_TX_MAX_COAL_BDS, sc->bge_tx_max_coal_bds);
	CSR_WRITE_4(sc, BGE_HCC_RX_COAL_TICKS_INT, 0);
	CSR_WRITE_4(sc, BGE_HCC_TX_COAL_TICKS_INT, 0);
	CSR_WRITE_4(sc, BGE_HCC_RX_MAX_COAL_BDS_INT, 0);
	CSR_WRITE_4(sc, BGE_HCC_TX_MAX_COAL_BDS_INT, 0);
	CSR_WRITE_4(sc, BGE_HCC_STATS_TICKS, sc->bge_stat_ticks);

	/* Set up address of statistics block */
	CSR_WRITE_4(sc, BGE_HCC_STATS_BASEADDR, BGE_STATS_BLOCK);
	CSR_WRITE_4(sc, BGE_HCC_STATS_ADDR_HI, 0);
	CSR_WRITE_4(sc, BGE_HCC_STATS_ADDR_LO,
		    BGE_RING_DMA_ADDR(sc, bge_info.bge_stats));

	/* Set up address of status block */
	CSR_WRITE_4(sc, BGE_HCC_STATUSBLK_BASEADDR, BGE_STATUS_BLOCK);
	CSR_WRITE_4(sc, BGE_HCC_STATUSBLK_ADDR_HI, 0);
	CSR_WRITE_4(sc, BGE_HCC_STATUSBLK_ADDR_LO,
		    BGE_RING_DMA_ADDR(sc, bge_status_block));
	sc->bge_rdata->bge_status_block.bge_idx[0].bge_rx_prod_idx = 0;
	sc->bge_rdata->bge_status_block.bge_idx[0].bge_tx_cons_idx = 0;

	/* Turn on host coalescing state machine */
	CSR_WRITE_4(sc, BGE_HCC_MODE, BGE_HCCMODE_ENABLE);

	/* Turn on RX BD completion state machine and enable attentions */
	CSR_WRITE_4(sc, BGE_RBDC_MODE,
	    BGE_RBDCMODE_ENABLE|BGE_RBDCMODE_ATTN);

	/* Turn on RX list placement state machine */
	CSR_WRITE_4(sc, BGE_RXLP_MODE, BGE_RXLPMODE_ENABLE);

	/* Turn on RX list selector state machine. */
	CSR_WRITE_4(sc, BGE_RXLS_MODE, BGE_RXLSMODE_ENABLE);

	/* Turn on DMA, clear stats */
	CSR_WRITE_4(sc, BGE_MAC_MODE, BGE_MACMODE_TXDMA_ENB|
	    BGE_MACMODE_RXDMA_ENB|BGE_MACMODE_RX_STATS_CLEAR|
	    BGE_MACMODE_TX_STATS_CLEAR|BGE_MACMODE_RX_STATS_ENB|
	    BGE_MACMODE_TX_STATS_ENB|BGE_MACMODE_FRMHDR_DMA_ENB|
	    (sc->bge_tbi ? BGE_PORTMODE_TBI : BGE_PORTMODE_MII));

	/* Set misc. local control, enable interrupts on attentions */
	CSR_WRITE_4(sc, BGE_MISC_LOCAL_CTL, BGE_MLC_INTR_ONATTN);

#ifdef notdef
	/* Assert GPIO pins for PHY reset */
	BGE_SETBIT(sc, BGE_MISC_LOCAL_CTL, BGE_MLC_MISCIO_OUT0|
	    BGE_MLC_MISCIO_OUT1|BGE_MLC_MISCIO_OUT2);
	BGE_SETBIT(sc, BGE_MISC_LOCAL_CTL, BGE_MLC_MISCIO_OUTEN0|
	    BGE_MLC_MISCIO_OUTEN1|BGE_MLC_MISCIO_OUTEN2);
#endif

	/* Turn on DMA completion state machine */
	CSR_WRITE_4(sc, BGE_DMAC_MODE, BGE_DMACMODE_ENABLE);

	/* Turn on write DMA state machine */
	CSR_WRITE_4(sc, BGE_WDMA_MODE,
	    BGE_WDMAMODE_ENABLE|BGE_WDMAMODE_ALL_ATTNS);

	/* Turn on read DMA state machine */
	CSR_WRITE_4(sc, BGE_RDMA_MODE,
	    BGE_RDMAMODE_ENABLE|BGE_RDMAMODE_ALL_ATTNS);

	/* Turn on RX data completion state machine */
	CSR_WRITE_4(sc, BGE_RDC_MODE, BGE_RDCMODE_ENABLE);

	/* Turn on RX BD initiator state machine */
	CSR_WRITE_4(sc, BGE_RBDI_MODE, BGE_RBDIMODE_ENABLE);

	/* Turn on RX data and RX BD initiator state machine */
	CSR_WRITE_4(sc, BGE_RDBDI_MODE, BGE_RDBDIMODE_ENABLE);

	/* Turn on Mbuf cluster free state machine */
	CSR_WRITE_4(sc, BGE_MBCF_MODE, BGE_MBCFMODE_ENABLE);

	/* Turn on send BD completion state machine */
	CSR_WRITE_4(sc, BGE_SBDC_MODE, BGE_SBDCMODE_ENABLE);

	/* Turn on send data completion state machine */
	CSR_WRITE_4(sc, BGE_SDC_MODE, BGE_SDCMODE_ENABLE);

	/* Turn on send data initiator state machine */
	CSR_WRITE_4(sc, BGE_SDI_MODE, BGE_SDIMODE_ENABLE);

	/* Turn on send BD initiator state machine */
	CSR_WRITE_4(sc, BGE_SBDI_MODE, BGE_SBDIMODE_ENABLE);

	/* Turn on send BD selector state machine */
	CSR_WRITE_4(sc, BGE_SRS_MODE, BGE_SRSMODE_ENABLE);

	CSR_WRITE_4(sc, BGE_SDI_STATS_ENABLE_MASK, 0x007FFFFF);
	CSR_WRITE_4(sc, BGE_SDI_STATS_CTL,
	    BGE_SDISTATSCTL_ENABLE|BGE_SDISTATSCTL_FASTER);

	/* init LED register */
	CSR_WRITE_4(sc, BGE_MAC_LED_CTL, 0x00000000);

	/* ack/clear link change events */
	CSR_WRITE_4(sc, BGE_MAC_STS, BGE_MACSTAT_SYNC_CHANGED|
	    BGE_MACSTAT_CFG_CHANGED);
	CSR_WRITE_4(sc, BGE_MI_STS, 0);

	/* Enable PHY auto polling (for MII/GMII only) */
	if (sc->bge_tbi) {
		CSR_WRITE_4(sc, BGE_MI_STS, BGE_MISTS_LINK);
	} else
		BGE_SETBIT(sc, BGE_MI_MODE, BGE_MIMODE_AUTOPOLL|10<<16);

	/* Enable link state change attentions. */
	BGE_SETBIT(sc, BGE_MAC_EVT_ENB, BGE_EVTENB_LINK_CHANGED);

	return(0);
}

/*
 * Probe for a Broadcom chip. Check the PCI vendor and device IDs
 * against our list and return its name if we find a match. Note
 * that since the Broadcom controller contains VPD support, we
 * can get the device name string from the controller itself instead
 * of the compiled-in string. This is a little slow, but it guarantees
 * we'll always announce the right product name.
 */
int
bge_probe(parent, match, aux)
	struct device *parent;
	void *match;
	void *aux;
{
	struct pci_attach_args *pa = (struct pci_attach_args *)aux;

	/*
	 * Various supported device vendors/types and their
	 * names. Note: the spec seems to indicate that the hardware
	 * still has Alteon's vendor ID burned into it, though it will
	 * always be overriden by the vendor ID in the EEPROM. Just to
	 * be safe, we cover all possibilities.
	 */

	if (PCI_VENDOR(pa->pa_id) == PCI_VENDOR_ALTEON &&
	    (PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_ALTEON_BCM5700 ||
	     PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_ALTEON_BCM5701))
		return (1);

	if (PCI_VENDOR(pa->pa_id) == PCI_VENDOR_BROADCOM &&
	    (PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_BROADCOM_BCM5700 ||
	     PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_BROADCOM_BCM5701))
		return (1);

	if (PCI_VENDOR(pa->pa_id) == PCI_VENDOR_SCHNEIDERKOCH &&
	    PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_SCHNEIDERKOCH_SK9D21)
		return (1);

	if (PCI_VENDOR(pa->pa_id) == PCI_VENDOR_3COM &&
	    PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_3COM_3C996)
		return (1);

	return (0);
}

void
bge_attach(parent, self, aux)
	struct device *parent, *self;
	void *aux;
{
	struct bge_softc	*sc = (struct bge_softc *)self;
	struct pci_attach_args	*pa = aux;
	pci_chipset_tag_t	pc = pa->pa_pc;
	pci_intr_handle_t	ih;
	const char		*intrstr = NULL;
	bus_addr_t		iobase;
	bus_size_t		iosize;
	bus_dma_segment_t	seg;
	int			s, rseg;
	u_int32_t		command;
	struct ifnet		*ifp;
	int			unit, error = 0;
	caddr_t			kva;

	s = splimp();

	sc->bge_pa = *pa;

	/*
	 * Map control/status registers.
	 */
	DPRINTFN(5, ("Map control/status regs\n"));
	command = pci_conf_read(pc, pa->pa_tag, PCI_COMMAND_STATUS_REG);
	command |= PCI_COMMAND_MEM_ENABLE | PCI_COMMAND_MASTER_ENABLE;
	pci_conf_write(pc, pa->pa_tag, PCI_COMMAND_STATUS_REG, command);
	command = pci_conf_read(pc, pa->pa_tag, PCI_COMMAND_STATUS_REG);

	if (!(command & PCI_COMMAND_MEM_ENABLE)) {
		printf("%s: failed to enable memory mapping!\n",
		    sc->bge_dev.dv_xname);
		error = ENXIO;
		goto fail;
	}

	DPRINTFN(5, ("pci_mem_find\n"));
	if (pci_mem_find(pc, pa->pa_tag, BGE_PCI_BAR0, &iobase,
			 &iosize, NULL)) {
		printf(": can't find mem space\n");
		goto fail;
	}

	DPRINTFN(5, ("bus_space_map\n"));
	if (bus_space_map(pa->pa_memt, iobase, iosize, 0, &sc->bge_bhandle)) {
		printf(": can't map mem space\n");
		goto fail;
	}

	sc->bge_btag = pa->pa_memt;

	DPRINTFN(5, ("pci_intr_map\n"));
	if (pci_intr_map(pa, &ih)) {
		printf(": couldn't map interrupt\n");
		goto fail;
	}

	DPRINTFN(5, ("pci_intr_string\n"));
	intrstr = pci_intr_string(pc, ih);

	DPRINTFN(5, ("pci_intr_establish\n"));
	sc->bge_intrhand = pci_intr_establish(pc, ih, IPL_NET, bge_intr, sc,
	    sc->bge_dev.dv_xname);

	if (sc->bge_intrhand == NULL) {
		printf(": couldn't establish interrupt");
		if (intrstr != NULL)
			printf(" at %s", intrstr);
		printf("\n");
		goto fail;
	}
	printf(": %s", intrstr);

	/* Try to reset the chip. */
	DPRINTFN(5, ("bge_reset\n"));
	bge_reset(sc);

	if (bge_chipinit(sc)) {
		printf("%s: chip initializatino failed\n",
		    sc->bge_dev.dv_xname);
		bge_release_resources(sc);
		error = ENXIO;
		goto fail;
	}

	/*
	 * Get station address from the EEPROM.
	 */
	if (bge_read_eeprom(sc, (caddr_t)&sc->arpcom.ac_enaddr,
	    BGE_EE_MAC_OFFSET + 2, ETHER_ADDR_LEN)) {
		printf("bge%d: failed to read station address\n", unit);
		bge_release_resources(sc);
		error = ENXIO;
		goto fail;
	}

	/*
	 * A Broadcom chip was detected. Inform the world.
	 */
	printf(": Ethernet address: %s\n",
	    ether_sprintf(sc->arpcom.ac_enaddr));

	/* Allocate the general information block and ring buffers. */
	sc->bge_dmatag = pa->pa_dmat;
	DPRINTFN(5, ("bus_dmamem_alloc\n"));
	if (bus_dmamem_alloc(sc->bge_dmatag, sizeof(struct bge_ring_data),
			     PAGE_SIZE, 0, &seg, 1, &rseg, BUS_DMA_NOWAIT)) {
		printf("%s: can't alloc rx buffers\n", sc->bge_dev.dv_xname);
		goto fail;
	}
	DPRINTFN(5, ("bus_dmamem_map\n"));
	if (bus_dmamem_map(sc->bge_dmatag, &seg, rseg,
			   sizeof(struct bge_ring_data), &kva,
			   BUS_DMA_NOWAIT)) {
		printf("%s: can't map dma buffers (%d bytes)\n",
		    sc->bge_dev.dv_xname, sizeof(struct bge_ring_data));
		bus_dmamem_free(sc->bge_dmatag, &seg, rseg);
		goto fail;
	}
	DPRINTFN(5, ("bus_dmamem_create\n"));
	if (bus_dmamap_create(sc->bge_dmatag, sizeof(struct bge_ring_data), 1,
	    sizeof(struct bge_ring_data), 0,
	    BUS_DMA_NOWAIT, &sc->bge_ring_map)) {
		printf("%s: can't create dma map\n", sc->bge_dev.dv_xname);
		bus_dmamem_unmap(sc->bge_dmatag, kva,
				 sizeof(struct bge_ring_data));
		bus_dmamem_free(sc->bge_dmatag, &seg, rseg);
		goto fail;
	}
	DPRINTFN(5, ("bus_dmamem_load\n"));
	if (bus_dmamap_load(sc->bge_dmatag, sc->bge_ring_map, kva,
			    sizeof(struct bge_ring_data), NULL,
			    BUS_DMA_NOWAIT)) {
		bus_dmamap_destroy(sc->bge_dmatag, sc->bge_ring_map);
		bus_dmamem_unmap(sc->bge_dmatag, kva,
				 sizeof(struct bge_ring_data));
		bus_dmamem_free(sc->bge_dmatag, &seg, rseg);
		goto fail;
	}

	DPRINTFN(5, ("bzero\n"));
	sc->bge_rdata = (struct bge_ring_data *)kva;

	bzero(sc->bge_rdata, sizeof(struct bge_ring_data));

	/* Try to allocate memory for jumbo buffers. */
	if (bge_alloc_jumbo_mem(sc)) {
		printf("%s: jumbo buffer allocation failed\n",
		    sc->bge_dev.dv_xname);
		error = ENXIO;
		goto fail;
	}

	/* Set default tuneable values. */
	sc->bge_stat_ticks = BGE_TICKS_PER_SEC;
	sc->bge_rx_coal_ticks = 150;
	sc->bge_tx_coal_ticks = 150;
	sc->bge_rx_max_coal_bds = 64;
	sc->bge_tx_max_coal_bds = 128;

	/* Set up ifnet structure */
	ifp = &sc->arpcom.ac_if;
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = bge_ioctl;
	ifp->if_output = ether_output;
	ifp->if_start = bge_start;
	ifp->if_watchdog = bge_watchdog;
	ifp->if_baudrate = 1000000000;
	ifp->if_mtu = ETHERMTU;
	ifp->if_snd.ifq_maxlen = BGE_TX_RING_CNT - 1;
	DPRINTFN(5, ("bcopy\n"));
	bcopy(sc->bge_dev.dv_xname, ifp->if_xname, IFNAMSIZ);

	/*
	 * Do MII setup.
	 */
	DPRINTFN(5, ("mii setup\n"));
	sc->bge_mii.mii_ifp = ifp;
	sc->bge_mii.mii_readreg = bge_miibus_readreg;
	sc->bge_mii.mii_writereg = bge_miibus_writereg;
	sc->bge_mii.mii_statchg = bge_miibus_statchg;

	/* The SysKonnect SK-9D41 is a 1000baseSX card. */
	if ((pci_conf_read(pa->pa_pc, pa->pa_tag, BGE_PCI_SUBSYS) >> 16) ==
	    SK_SUBSYSID_9D41)
		sc->bge_tbi = 1;

	/*
	 * Do transceiver setup.
	 */
	ifmedia_init(&sc->bge_mii.mii_media, 0, bge_ifmedia_upd,
		     bge_ifmedia_sts);
	mii_attach(&sc->bge_dev, &sc->bge_mii, 0xffffffff,
		   MII_PHY_ANY, MII_OFFSET_ANY, 0);

	if (LIST_FIRST(&sc->bge_mii.mii_phys) == NULL) {
		printf("%s: no PHY found!\n", sc->bge_dev.dv_xname);
		ifmedia_add(&sc->bge_mii.mii_media, IFM_ETHER|IFM_MANUAL,
			    0, NULL);
		ifmedia_set(&sc->bge_mii.mii_media, IFM_ETHER|IFM_MANUAL);
	} else
		ifmedia_set(&sc->bge_mii.mii_media, IFM_ETHER|IFM_AUTO);

	/*
	 * Call MI attach routine.
	 */
	DPRINTFN(5, ("if_attach\n"));
	if_attach(ifp);
	DPRINTFN(5, ("ether_ifattach\n"));
	ether_ifattach(ifp);
	DPRINTFN(5, ("timeout_set\n"));
	timeout_set(&sc->bge_timeout, bge_tick, sc);
fail:
	splx(s);
}

void
bge_release_resources(sc)
	struct bge_softc *sc;
{
	if (sc->bge_vpd_prodname != NULL)
		free(sc->bge_vpd_prodname, M_DEVBUF);

	if (sc->bge_vpd_readonly != NULL)
		free(sc->bge_vpd_readonly, M_DEVBUF);

#ifdef fake
	if (sc->bge_intrhand != NULL)
		bus_teardown_intr(dev, sc->bge_irq, sc->bge_intrhand);

	if (sc->bge_irq != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->bge_irq);

	if (sc->bge_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY,
		    BGE_PCI_BAR0, sc->bge_res);

	if (sc->bge_rdata != NULL)
		contigfree(sc->bge_rdata,
		    sizeof(struct bge_ring_data), M_DEVBUF);
#endif
}

void
bge_reset(sc)
	struct bge_softc *sc;
{
	struct pci_attach_args *pa = &sc->bge_pa;
	u_int32_t cachesize, command, pcistate;
	int i, val = 0;

	/* Save some important PCI state. */
	cachesize = pci_conf_read(pa->pa_pc, pa->pa_tag, BGE_PCI_CACHESZ);
	command = pci_conf_read(pa->pa_pc, pa->pa_tag, BGE_PCI_CMD);
	pcistate = pci_conf_read(pa->pa_pc, pa->pa_tag, BGE_PCI_PCISTATE);

	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_MISC_CTL,
	    BGE_PCIMISCCTL_INDIRECT_ACCESS|BGE_PCIMISCCTL_MASK_PCI_INTR|
	    BGE_PCIMISCCTL_ENDIAN_WORDSWAP|BGE_PCIMISCCTL_PCISTATE_RW);

	/* Issue global reset */
	bge_writereg_ind(sc, BGE_MISC_CFG,
	    BGE_MISCCFG_RESET_CORE_CLOCKS|(65<<1));

	DELAY(1000);

	/* Reset some of the PCI state that got zapped by reset */
	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_MISC_CTL,
	    BGE_PCIMISCCTL_INDIRECT_ACCESS|BGE_PCIMISCCTL_MASK_PCI_INTR|
	    BGE_PCIMISCCTL_ENDIAN_WORDSWAP|BGE_PCIMISCCTL_PCISTATE_RW);
	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_CACHESZ, cachesize);
	pci_conf_write(pa->pa_pc, pa->pa_tag, BGE_PCI_CMD, command);
	bge_writereg_ind(sc, BGE_MISC_CFG, (65 << 1));

	/*
	 * Prevent PXE restart: write a magic number to the
	 * general communications memory at 0xB50.
	 */
	bge_writemem_ind(sc, BGE_SOFTWARE_GENCOMM, BGE_MAGIC_NUMBER);
	/*
	 * Poll the value location we just wrote until
	 * we see the 1's complement of the magic number.
	 * This indicates that the firmware initialization
	 * is complete.
	 */
	for (i = 0; i < BGE_TIMEOUT; i++) {
		val = bge_readmem_ind(sc, BGE_SOFTWARE_GENCOMM);
		if (val == ~BGE_MAGIC_NUMBER)
			break;
		DELAY(10);
	}

	if (i == BGE_TIMEOUT) {
		printf("%s: firmware handshake timed out\n",
		    sc->bge_dev.dv_xname);
		return;
	}

	/*
	 * XXX Wait for the value of the PCISTATE register to
	 * return to its original pre-reset state. This is a
	 * fairly good indicator of reset completion. If we don't
	 * wait for the reset to fully complete, trying to read
	 * from the device's non-PCI registers may yield garbage
	 * results.
	 */
	for (i = 0; i < BGE_TIMEOUT; i++) {
		if (pci_conf_read(pa->pa_pc, pa->pa_tag, BGE_PCI_PCISTATE) ==
		    pcistate)
			break;
		DELAY(10);
	}

	/* Enable memory arbiter. */
	CSR_WRITE_4(sc, BGE_MARB_MODE, BGE_MARBMODE_ENABLE);

	/* Fix up byte swapping */
	CSR_WRITE_4(sc, BGE_MODE_CTL, BGE_MODECTL_BYTESWAP_NONFRAME|
	    BGE_MODECTL_BYTESWAP_DATA);

	CSR_WRITE_4(sc, BGE_MAC_MODE, 0);

	DELAY(10000);
}

/*
 * Frame reception handling. This is called if there's a frame
 * on the receive return list.
 *
 * Note: we have to be able to handle two possibilities here:
 * 1) the frame is from the jumbo recieve ring
 * 2) the frame is from the standard receive ring
 */

void
bge_rxeof(sc)
	struct bge_softc *sc;
{
	struct ifnet *ifp;
	int stdcnt = 0, jumbocnt = 0;

	ifp = &sc->arpcom.ac_if;

	while(sc->bge_rx_saved_considx !=
	    sc->bge_rdata->bge_status_block.bge_idx[0].bge_rx_prod_idx) {
		struct bge_rx_bd	*cur_rx;
		u_int32_t		rxidx;
		struct mbuf		*m = NULL;
#if NVLAN > 0
		u_int16_t		vlan_tag = 0;
		int			have_tag = 0;
#endif
#ifdef BGE_CHECKSUM
		int			sumflags = 0;
#endif

		cur_rx = &sc->bge_rdata->
			bge_rx_return_ring[sc->bge_rx_saved_considx];

		rxidx = cur_rx->bge_idx;
		BGE_INC(sc->bge_rx_saved_considx, BGE_RETURN_RING_CNT);

#if NVLAN > 0
		if (cur_rx->bge_flags & BGE_RXBDFLAG_VLAN_TAG) {
			have_tag = 1;
			vlan_tag = cur_rx->bge_vlan_tag;
		}
#endif

		if (cur_rx->bge_flags & BGE_RXBDFLAG_JUMBO_RING) {
			BGE_INC(sc->bge_jumbo, BGE_JUMBO_RX_RING_CNT);
			m = sc->bge_cdata.bge_rx_jumbo_chain[rxidx];
			sc->bge_cdata.bge_rx_jumbo_chain[rxidx] = NULL;
			jumbocnt++;
			if (cur_rx->bge_flags & BGE_RXBDFLAG_ERROR) {
				ifp->if_ierrors++;
				bge_newbuf_jumbo(sc, sc->bge_jumbo, m);
				continue;
			}
			if (bge_newbuf_jumbo(sc, sc->bge_jumbo,
					     NULL)== ENOBUFS) {
				ifp->if_ierrors++;
				bge_newbuf_jumbo(sc, sc->bge_jumbo, m);
				continue;
			}
		} else {
			BGE_INC(sc->bge_std, BGE_STD_RX_RING_CNT);
			m = sc->bge_cdata.bge_rx_std_chain[rxidx];
			sc->bge_cdata.bge_rx_std_chain[rxidx] = NULL;
			bus_dmamap_unload(sc->bge_dmatag,
					  sc->bge_cdata.bge_rx_std_map[rxidx]);
			stdcnt++;
			if (cur_rx->bge_flags & BGE_RXBDFLAG_ERROR) {
				ifp->if_ierrors++;
				bge_newbuf_std(sc, sc->bge_std, m);
				continue;
			}
			if (bge_newbuf_std(sc, sc->bge_std,
			    NULL) == ENOBUFS) {
				ifp->if_ierrors++;
				bge_newbuf_std(sc, sc->bge_std, m);
				continue;
			}
		}

		ifp->if_ipackets++;
		m->m_pkthdr.len = m->m_len = cur_rx->bge_len;
		m->m_pkthdr.rcvif = ifp;

#if NBPFILTER > 0
		/*
		 * Handle BPF listeners. Let the BPF user see the packet.
		 */
		if (ifp->if_bpf)
			bpf_mtap(ifp->if_bpf, m);
#endif

#ifdef BGE_CHECKSUM
		if ((cur_rx->bge_ip_csum ^ 0xffff) == 0)
			sumflags |= M_IPV4_CSUM_IN_OK;
		else
			sumflags |= M_IPV4_CSUM_IN_BAD;
#if 0
		if (cur_rx->bge_flags & BGE_RXBDFLAG_TCP_UDP_CSUM) {
			m->m_pkthdr.csum_data =
				cur_rx->bge_tcp_udp_csum;
			m->m_pkthdr.csum_flags |= CSUM_DATA_VALID;
		}
#endif
		m->m_pkthdr.csum = sumflags;
		sumflags = 0;
#endif

#if NVLAN > 0
		/*
		 * If we received a packet with a vlan tag, pass it
		 * to vlan_input() instead of ether_input().
		 */
		if (have_tag) {
			vlan_input_tag(m, vlan_tag);
			have_tag = vlan_tag = 0;
			continue;
		}
#endif
		ether_input_mbuf(ifp, m);
	}

	CSR_WRITE_4(sc, BGE_MBX_RX_CONS0_LO, sc->bge_rx_saved_considx);
	if (stdcnt)
		CSR_WRITE_4(sc, BGE_MBX_RX_STD_PROD_LO, sc->bge_std);
	if (jumbocnt)
		CSR_WRITE_4(sc, BGE_MBX_RX_JUMBO_PROD_LO, sc->bge_jumbo);
}

void
bge_txeof(sc)
	struct bge_softc *sc;
{
	struct bge_tx_bd *cur_tx = NULL;
	struct ifnet *ifp;

	ifp = &sc->arpcom.ac_if;

	/*
	 * Go through our tx ring and free mbufs for those
	 * frames that have been sent.
	 */
	while (sc->bge_tx_saved_considx !=
	    sc->bge_rdata->bge_status_block.bge_idx[0].bge_tx_cons_idx) {
		u_int32_t		idx = 0;

		idx = sc->bge_tx_saved_considx;
		cur_tx = &sc->bge_rdata->bge_tx_ring[idx];
		if (cur_tx->bge_flags & BGE_TXBDFLAG_END)
			ifp->if_opackets++;
		if (sc->bge_cdata.bge_tx_chain[idx] != NULL) {
			m_freem(sc->bge_cdata.bge_tx_chain[idx]);
			sc->bge_cdata.bge_tx_chain[idx] = NULL;
			bus_dmamap_unload(sc->bge_dmatag,
					  sc->bge_cdata.bge_tx_map[idx]);
		}
		sc->bge_txcnt--;
		BGE_INC(sc->bge_tx_saved_considx, BGE_TX_RING_CNT);
		ifp->if_timer = 0;
	}

	if (cur_tx != NULL)
		ifp->if_flags &= ~IFF_OACTIVE;
}

int
bge_intr(xsc)
	void *xsc;
{
	struct bge_softc *sc;
	struct ifnet *ifp;

	sc = xsc;
	ifp = &sc->arpcom.ac_if;

#ifdef notdef
	/* Avoid this for now -- checking this register is expensive. */
	/* Make sure this is really our interrupt. */
	if (!(CSR_READ_4(sc, BGE_MISC_LOCAL_CTL) & BGE_MLC_INTR_STATE))
		return (0);
#endif
	/* Ack interrupt and stop others from occuring. */
	CSR_WRITE_4(sc, BGE_MBX_IRQ0_LO, 1);

	/* Process link state changes. */
	if (sc->bge_rdata->bge_status_block.bge_status &
	    BGE_STATFLAG_LINKSTATE_CHANGED) {
		sc->bge_link = 0;
		timeout_del(&sc->bge_timeout);
		bge_tick(sc);
		/* ack the event to clear/reset it */
		CSR_WRITE_4(sc, BGE_MAC_STS, BGE_MACSTAT_SYNC_CHANGED|
		    BGE_MACSTAT_CFG_CHANGED);
		CSR_WRITE_4(sc, BGE_MI_STS, 0);
	}

	if (ifp->if_flags & IFF_RUNNING) {
		/* Check RX return ring producer/consumer */
		bge_rxeof(sc);

		/* Check TX ring producer/consumer */
		bge_txeof(sc);
	}

	bge_handle_events(sc);

	/* Re-enable interrupts. */
	CSR_WRITE_4(sc, BGE_MBX_IRQ0_LO, 0);

	if (ifp->if_flags & IFF_RUNNING && ifp->if_snd.ifq_head != NULL)
		bge_start(ifp);

	return (1);
}

void
bge_tick(xsc)
	void *xsc;
{
	struct bge_softc *sc = xsc;
	struct mii_data *mii = &sc->bge_mii;
	struct ifmedia *ifm = NULL;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int s;

	s = splimp();

	bge_stats_update(sc);
	timeout_add(&sc->bge_timeout, hz);
	if (sc->bge_link) {
		splx(s);
		return;
	}

	if (sc->bge_tbi) {
		ifm = &sc->bge_ifmedia;
		if (CSR_READ_4(sc, BGE_MAC_STS) &
		    BGE_MACSTAT_TBI_PCS_SYNCHED) {
			sc->bge_link++;
			CSR_WRITE_4(sc, BGE_MAC_STS, 0xFFFFFFFF);
			printf("%s: gigabit link up\n", sc->bge_dev.dv_xname);
			if (ifp->if_snd.ifq_head != NULL)
				bge_start(ifp);
		}
		splx(s);
		return;
	}

	mii_tick(mii);

	if (!sc->bge_link && mii->mii_media_status & IFM_ACTIVE &&
	    IFM_SUBTYPE(mii->mii_media_active) != IFM_NONE) {
		sc->bge_link++;
		if (IFM_SUBTYPE(mii->mii_media_active) == IFM_1000_TX ||
		    IFM_SUBTYPE(mii->mii_media_active) == IFM_1000_SX)
			printf("%s: gigabit link up\n", sc->bge_dev.dv_xname);
		if (ifp->if_snd.ifq_head != NULL)
			bge_start(ifp);
	}

	splx(s);
}

void
bge_stats_update(sc)
	struct bge_softc *sc;
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	bus_size_t stats = BGE_MEMWIN_START + BGE_STATS_BLOCK;

#define READ_STAT(sc, stats, stat) \
	  CSR_READ_4(sc, stats + offsetof(struct bge_stats, stat))

	ifp->if_collisions +=
	  (READ_STAT(sc, stats, dot3StatsSingleCollisionFrames.bge_addr_lo) +
	   READ_STAT(sc, stats, dot3StatsMultipleCollisionFrames.bge_addr_lo) +
	   READ_STAT(sc, stats, dot3StatsExcessiveCollisions.bge_addr_lo) +
	   READ_STAT(sc, stats, dot3StatsLateCollisions.bge_addr_lo)) -
	  ifp->if_collisions;

#undef READ_STAT

#ifdef notdef
	ifp->if_collisions +=
	   (sc->bge_rdata->bge_info.bge_stats.dot3StatsSingleCollisionFrames +
	   sc->bge_rdata->bge_info.bge_stats.dot3StatsMultipleCollisionFrames +
	   sc->bge_rdata->bge_info.bge_stats.dot3StatsExcessiveCollisions +
	   sc->bge_rdata->bge_info.bge_stats.dot3StatsLateCollisions) -
	   ifp->if_collisions;
#endif
}

/*
 * Encapsulate an mbuf chain in the tx ring  by coupling the mbuf data
 * pointers to descriptors.
 */
int
bge_encap(sc, m_head, txidx)
	struct bge_softc *sc;
	struct mbuf *m_head;
	u_int32_t *txidx;
{
	struct bge_tx_bd	*f = NULL;
	struct mbuf		*m;
	u_int32_t		frag, cur, cnt = 0;
	u_int16_t		csum_flags = 0;
	bus_dmamap_t		txmap;
	int			i = 0;
#if NVLAN > 0
	struct ifvlan		*ifv = NULL;

	if ((m_head->m_flags & (M_PROTO1|M_PKTHDR)) == (M_PROTO1|M_PKTHDR) &&
	    m_head->m_pkthdr.rcvif != NULL)
		ifv = m_head->m_pkthdr.rcvif->if_softc;
#endif

	m = m_head;
	cur = frag = *txidx;

#ifdef BGE_CHECKSUM
	if (m_head->m_pkthdr.csum) {
		if (m_head->m_pkthdr.csum & M_IPV4_CSUM_OUT)
			csum_flags |= BGE_TXBDFLAG_IP_CSUM;
		if (m_head->m_pkthdr.csum & (M_TCPV4_CSUM_OUT |
					     M_UDPV4_CSUM_OUT))
			csum_flags |= BGE_TXBDFLAG_TCP_UDP_CSUM;
#ifdef fake
		if (m_head->m_flags & M_LASTFRAG)
			csum_flags |= BGE_TXBDFLAG_IP_FRAG_END;
		else if (m_head->m_flags & M_FRAG)
			csum_flags |= BGE_TXBDFLAG_IP_FRAG;
#endif
	}
#endif

	/*
	 * Start packing the mbufs in this chain into
	 * the fragment pointers. Stop when we run out
	 * of fragments or hit the end of the mbuf chain.
	 */
	txmap = sc->bge_cdata.bge_tx_map[frag];
	if (bus_dmamap_load_mbuf(sc->bge_dmatag, txmap, m,
				 BUS_DMA_NOWAIT))
		return(ENOBUFS);

	for (m = m_head; m != NULL; m = m->m_next) {
		if (m->m_len != 0) {
			f = &sc->bge_rdata->bge_tx_ring[frag];
			if (sc->bge_cdata.bge_tx_chain[frag] != NULL)
				break;
			BGE_HOSTADDR(f->bge_addr) =
				txmap->dm_segs[i++].ds_addr;
			f->bge_len = m->m_len;
			f->bge_flags = csum_flags;
#if NVLAN > 0
			if (ifv != NULL) {
				f->bge_flags |= BGE_TXBDFLAG_VLAN_TAG;
				f->bge_vlan_tag = ifv->ifv_tag;
			} else {
				f->bge_vlan_tag = 0;
			}
#endif
			/*
			 * Sanity check: avoid coming within 16 descriptors
			 * of the end of the ring.
			 */
			if ((BGE_TX_RING_CNT - (sc->bge_txcnt + cnt)) < 16)
				return(ENOBUFS);
			cur = frag;
			BGE_INC(frag, BGE_TX_RING_CNT);
			cnt++;
		}
	}

	if (m != NULL)
		return(ENOBUFS);

	if (frag == sc->bge_tx_saved_considx)
		return(ENOBUFS);

	sc->bge_rdata->bge_tx_ring[cur].bge_flags |= BGE_TXBDFLAG_END;
	sc->bge_cdata.bge_tx_chain[cur] = m_head;
	sc->bge_txcnt += cnt;

	*txidx = frag;

	return(0);
}

/*
 * Main transmit routine. To avoid having to do mbuf copies, we put pointers
 * to the mbuf data regions directly in the transmit descriptors.
 */
void
bge_start(ifp)
	struct ifnet *ifp;
{
	struct bge_softc *sc;
	struct mbuf *m_head = NULL;
	u_int32_t prodidx = 0;

	sc = ifp->if_softc;

	if (!sc->bge_link && ifp->if_snd.ifq_len < 10)
		return;

	prodidx = CSR_READ_4(sc, BGE_MBX_TX_HOST_PROD0_LO);

	while(sc->bge_cdata.bge_tx_chain[prodidx] == NULL) {
		IF_DEQUEUE(&ifp->if_snd, m_head);
		if (m_head == NULL)
			break;

		/*
		 * XXX
		 * safety overkill.  If this is a fragmented packet chain
		 * with delayed TCP/UDP checksums, then only encapsulate
		 * it if we have enough descriptors to handle the entire
		 * chain at once.
		 * (paranoia -- may not actually be needed)
		 */
#ifdef fake
		if (m_head->m_flags & M_FIRSTFRAG &&
		    m_head->m_pkthdr.csum_flags & (CSUM_DELAY_DATA)) {
			if ((BGE_TX_RING_CNT - sc->bge_txcnt) <
			    m_head->m_pkthdr.csum_data + 16) {
				IF_PREPEND(&ifp->if_snd, m_head);
				ifp->if_flags |= IFF_OACTIVE;
				break;
			}
		}
#endif

		/*
		 * Pack the data into the transmit ring. If we
		 * don't have room, set the OACTIVE flag and wait
		 * for the NIC to drain the ring.
		 */
		if (bge_encap(sc, m_head, &prodidx)) {
			IF_PREPEND(&ifp->if_snd, m_head);
			ifp->if_flags |= IFF_OACTIVE;
			break;
		}

		/*
		 * If there's a BPF listener, bounce a copy of this frame
		 * to him.
		 */
		if (ifp->if_bpf)
			bpf_mtap(ifp->if_bpf, m_head);
	}

	/* Transmit */
	CSR_WRITE_4(sc, BGE_MBX_TX_HOST_PROD0_LO, prodidx);

	/*
	 * Set a timeout in case the chip goes out to lunch.
	 */
	ifp->if_timer = 5;
}

/*
 * If we have a BCM5400 or BCM5401 PHY, we need to properly
 * program its internal DSP. Failing to do this can result in
 * massive packet loss at 1Gb speeds.
 */
void
bge_phy_hack(sc)
	struct bge_softc *sc;
{
	struct bge_bcom_hack bhack[] = {
	{ BRGPHY_MII_AUXCTL, 0x4C20 },
	{ BRGPHY_MII_DSP_ADDR_REG, 0x0012 },
	{ BRGPHY_MII_DSP_RW_PORT, 0x1804 },
	{ BRGPHY_MII_DSP_ADDR_REG, 0x0013 },
	{ BRGPHY_MII_DSP_RW_PORT, 0x1204 },
	{ BRGPHY_MII_DSP_ADDR_REG, 0x8006 },
	{ BRGPHY_MII_DSP_RW_PORT, 0x0132 },
	{ BRGPHY_MII_DSP_ADDR_REG, 0x8006 },
	{ BRGPHY_MII_DSP_RW_PORT, 0x0232 },
	{ BRGPHY_MII_DSP_ADDR_REG, 0x201F },
	{ BRGPHY_MII_DSP_RW_PORT, 0x0A20 },
	{ 0, 0 } };
	u_int16_t vid, did;
	int i;

	vid = bge_miibus_readreg(&sc->bge_dev, 1, MII_PHYIDR1);
	did = bge_miibus_readreg(&sc->bge_dev, 1, MII_PHYIDR2);

	if (MII_OUI(vid, did) == MII_OUI_xxBROADCOM &&
	    (MII_MODEL(did) == MII_MODEL_xxBROADCOM_BCM5400 ||
	     MII_MODEL(did) == MII_MODEL_xxBROADCOM_BCM5401)) {
		i = 0;
		while (bhack[i].reg) {
			bge_miibus_writereg(&sc->bge_dev, 1, bhack[i].reg,
					    bhack[i].val);
			i++;
		}
	}
}

void
bge_init(xsc)
	void *xsc;
{
	struct bge_softc *sc = xsc;
	struct ifnet *ifp;
	u_int16_t *m;
	int s;

	s = splimp();

	ifp = &sc->arpcom.ac_if;

	if (ifp->if_flags & IFF_RUNNING)
		return;

	/* Cancel pending I/O and flush buffers. */
	bge_stop(sc);
	bge_reset(sc);
	bge_chipinit(sc);

	/*
	 * Init the various state machines, ring
	 * control blocks and firmware.
	 */
	if (bge_blockinit(sc)) {
		printf("%s: initialization failure\n", sc->bge_dev.dv_xname);
		splx(s);
		return;
	}

	ifp = &sc->arpcom.ac_if;

	/* Specify MTU. */
	CSR_WRITE_4(sc, BGE_RX_MTU, ifp->if_mtu +
	    ETHER_HDR_LEN + ETHER_CRC_LEN);

	/* Load our MAC address. */
	m = (u_int16_t *)&sc->arpcom.ac_enaddr[0];
	CSR_WRITE_4(sc, BGE_MAC_ADDR1_LO, htons(m[0]));
	CSR_WRITE_4(sc, BGE_MAC_ADDR1_HI, (htons(m[1]) << 16) | htons(m[2]));

	/* Enable or disable promiscuous mode as needed. */
	if (ifp->if_flags & IFF_PROMISC) {
		BGE_SETBIT(sc, BGE_RX_MODE, BGE_RXMODE_RX_PROMISC);
	} else {
		BGE_CLRBIT(sc, BGE_RX_MODE, BGE_RXMODE_RX_PROMISC);
	}

	/* Program multicast filter. */
	bge_setmulti(sc);

	/* Init RX ring. */
	bge_init_rx_ring_std(sc);

	/* Init jumbo RX ring. */
	if (ifp->if_mtu > (ETHERMTU + ETHER_HDR_LEN + ETHER_CRC_LEN))
		bge_init_rx_ring_jumbo(sc);

	/* Init our RX return ring index */
	sc->bge_rx_saved_considx = 0;

	/* Init TX ring. */
	bge_init_tx_ring(sc);

	/* Turn on transmitter */
	BGE_SETBIT(sc, BGE_TX_MODE, BGE_TXMODE_ENABLE);

	/* Turn on receiver */
	BGE_SETBIT(sc, BGE_RX_MODE, BGE_RXMODE_ENABLE);

	/* Tell firmware we're alive. */
	BGE_SETBIT(sc, BGE_MODE_CTL, BGE_MODECTL_STACKUP);

	/* Enable host interrupts. */
	BGE_SETBIT(sc, BGE_PCI_MISC_CTL, BGE_PCIMISCCTL_CLEAR_INTA);
	BGE_CLRBIT(sc, BGE_PCI_MISC_CTL, BGE_PCIMISCCTL_MASK_PCI_INTR);
	CSR_WRITE_4(sc, BGE_MBX_IRQ0_LO, 0);

	bge_ifmedia_upd(ifp);

	ifp->if_flags |= IFF_RUNNING;
	ifp->if_flags &= ~IFF_OACTIVE;

	splx(s);

	timeout_add(&sc->bge_timeout, hz);
}

/*
 * Set media options.
 */
int
bge_ifmedia_upd(ifp)
	struct ifnet *ifp;
{
	struct bge_softc *sc = ifp->if_softc;
	struct mii_data *mii = &sc->bge_mii;
	struct ifmedia *ifm = &sc->bge_ifmedia;

	/* If this is a 1000baseX NIC, enable the TBI port. */
	if (sc->bge_tbi) {
		if (IFM_TYPE(ifm->ifm_media) != IFM_ETHER)
			return(EINVAL);
		switch(IFM_SUBTYPE(ifm->ifm_media)) {
		case IFM_AUTO:
			break;
		case IFM_1000_SX:
			if ((ifm->ifm_media & IFM_GMASK) == IFM_FDX) {
				BGE_CLRBIT(sc, BGE_MAC_MODE,
				    BGE_MACMODE_HALF_DUPLEX);
			} else {
				BGE_SETBIT(sc, BGE_MAC_MODE,
				    BGE_MACMODE_HALF_DUPLEX);
			}
			break;
		default:
			return(EINVAL);
		}
		return(0);
	}

	sc->bge_link = 0;
	if (mii->mii_instance) {
		struct mii_softc *miisc;
		for (miisc = LIST_FIRST(&mii->mii_phys); miisc != NULL;
		    miisc = LIST_NEXT(miisc, mii_list))
			mii_phy_reset(miisc);
	}
	bge_phy_hack(sc);
	mii_mediachg(mii);

	return(0);
}

/*
 * Report current media status.
 */
void
bge_ifmedia_sts(ifp, ifmr)
	struct ifnet *ifp;
	struct ifmediareq *ifmr;
{
	struct bge_softc *sc = ifp->if_softc;
	struct mii_data *mii = &sc->bge_mii;

	if (sc->bge_tbi) {
		ifmr->ifm_status = IFM_AVALID;
		ifmr->ifm_active = IFM_ETHER;
		if (CSR_READ_4(sc, BGE_MAC_STS) &
		    BGE_MACSTAT_TBI_PCS_SYNCHED)
			ifmr->ifm_status |= IFM_ACTIVE;
		ifmr->ifm_active |= IFM_1000_SX;
		if (CSR_READ_4(sc, BGE_MAC_MODE) & BGE_MACMODE_HALF_DUPLEX)
			ifmr->ifm_active |= IFM_HDX;
		else
			ifmr->ifm_active |= IFM_FDX;
		return;
	}

	mii_pollstat(mii);
	ifmr->ifm_active = mii->mii_media_active;
	ifmr->ifm_status = mii->mii_media_status;
}

int
bge_ioctl(ifp, command, data)
	struct ifnet *ifp;
	u_long command;
	caddr_t data;
{
	struct bge_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *) data;
	struct ifaddr *ifa = (struct ifaddr *)data;
	int s, error = 0;
	struct mii_data *mii;

	s = splimp();

	if ((error = ether_ioctl(ifp, &sc->arpcom, command, data)) > 0) {
		splx(s);
		return (error);
	}

	switch(command) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		switch (ifa->ifa_addr->sa_family) {
#ifdef INET
		case AF_INET:
			bge_init(sc);
			arp_ifinit(&sc->arpcom, ifa);
			break;
#endif /* INET */
		default:
			bge_init(sc);
			break;
		}
		break;
	case SIOCSIFMTU:
		if (ifr->ifr_mtu > BGE_JUMBO_MTU)
			error = EINVAL;
		else
			ifp->if_mtu = ifr->ifr_mtu;
		break;
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			/*
			 * If only the state of the PROMISC flag changed,
			 * then just use the 'set promisc mode' command
			 * instead of reinitializing the entire NIC. Doing
			 * a full re-init means reloading the firmware and
			 * waiting for it to start up, which may take a
			 * second or two.
			 */
			if (ifp->if_flags & IFF_RUNNING &&
			    ifp->if_flags & IFF_PROMISC &&
			    !(sc->bge_if_flags & IFF_PROMISC)) {
				BGE_SETBIT(sc, BGE_RX_MODE,
				    BGE_RXMODE_RX_PROMISC);
			} else if (ifp->if_flags & IFF_RUNNING &&
			    !(ifp->if_flags & IFF_PROMISC) &&
			    sc->bge_if_flags & IFF_PROMISC) {
				BGE_CLRBIT(sc, BGE_RX_MODE,
				    BGE_RXMODE_RX_PROMISC);
			} else
				bge_init(sc);
		} else {
			if (ifp->if_flags & IFF_RUNNING) {
				bge_stop(sc);
			}
		}
		sc->bge_if_flags = ifp->if_flags;
		error = 0;
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if (ifp->if_flags & IFF_RUNNING) {
			bge_setmulti(sc);
			error = 0;
		}
		break;
	case SIOCSIFMEDIA:
	case SIOCGIFMEDIA:
		if (sc->bge_tbi) {
			error = ifmedia_ioctl(ifp, ifr, &sc->bge_ifmedia,
			    command);
		} else {
			mii = &sc->bge_mii;
			error = ifmedia_ioctl(ifp, ifr, &mii->mii_media,
			    command);
		}
		error = 0;
		break;
	default:
		error = EINVAL;
		break;
	}

	(void)splx(s);

	return(error);
}

void
bge_watchdog(ifp)
	struct ifnet *ifp;
{
	struct bge_softc *sc;

	sc = ifp->if_softc;

	printf("%s: watchdog timeout -- resetting\n", sc->bge_dev.dv_xname);

	ifp->if_flags &= ~IFF_RUNNING;
	bge_init(sc);

	ifp->if_oerrors++;
}

/*
 * Stop the adapter and free any mbufs allocated to the
 * RX and TX lists.
 */
void
bge_stop(sc)
	struct bge_softc *sc;
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct ifmedia_entry *ifm;
	struct mii_data *mii;
	int mtmp, itmp;

	timeout_del(&sc->bge_timeout);

	/*
	 * Disable all of the receiver blocks
	 */
	BGE_CLRBIT(sc, BGE_RX_MODE, BGE_RXMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_RBDI_MODE, BGE_RBDIMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_RXLP_MODE, BGE_RXLPMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_RXLS_MODE, BGE_RXLSMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_RDBDI_MODE, BGE_RBDIMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_RDC_MODE, BGE_RDCMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_RBDC_MODE, BGE_RBDCMODE_ENABLE);

	/*
	 * Disable all of the transmit blocks
	 */
	BGE_CLRBIT(sc, BGE_SRS_MODE, BGE_SRSMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_SBDI_MODE, BGE_SBDIMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_SDI_MODE, BGE_SDIMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_RDMA_MODE, BGE_RDMAMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_SDC_MODE, BGE_SDCMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_DMAC_MODE, BGE_DMACMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_SBDC_MODE, BGE_SBDCMODE_ENABLE);

	/*
	 * Shut down all of the memory managers and related
	 * state machines.
	 */
	BGE_CLRBIT(sc, BGE_HCC_MODE, BGE_HCCMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_WDMA_MODE, BGE_WDMAMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_MBCF_MODE, BGE_MBCFMODE_ENABLE);
	CSR_WRITE_4(sc, BGE_FTQ_RESET, 0xFFFFFFFF);
	CSR_WRITE_4(sc, BGE_FTQ_RESET, 0);
	BGE_CLRBIT(sc, BGE_BMAN_MODE, BGE_BMANMODE_ENABLE);
	BGE_CLRBIT(sc, BGE_MARB_MODE, BGE_MARBMODE_ENABLE);

	/* Disable host interrupts. */
	BGE_SETBIT(sc, BGE_PCI_MISC_CTL, BGE_PCIMISCCTL_MASK_PCI_INTR);
	CSR_WRITE_4(sc, BGE_MBX_IRQ0_LO, 1);

	/*
	 * Tell firmware we're shutting down.
	 */
	BGE_CLRBIT(sc, BGE_MODE_CTL, BGE_MODECTL_STACKUP);

	/* Free the RX lists. */
	bge_free_rx_ring_std(sc);

	/* Free jumbo RX list. */
	bge_free_rx_ring_jumbo(sc);

	/* Free TX buffers. */
	bge_free_tx_ring(sc);

	/*
	 * Isolate/power down the PHY, but leave the media selection
	 * unchanged so that things will be put back to normal when
	 * we bring the interface back up.
	 */
	if (!sc->bge_tbi) {
		mii = &sc->bge_mii;
		itmp = ifp->if_flags;
		ifp->if_flags |= IFF_UP;
		ifm = mii->mii_media.ifm_cur;
		mtmp = ifm->ifm_media;
		ifm->ifm_media = IFM_ETHER|IFM_NONE;
		mii_mediachg(mii);
		ifm->ifm_media = mtmp;
		ifp->if_flags = itmp;
	}

	sc->bge_link = 0;

	sc->bge_tx_saved_considx = BGE_TXCONS_UNSET;

	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);
}

/*
 * Stop all chip I/O so that the kernel's probe routines don't
 * get confused by errant DMAs when rebooting.
 */
void
bge_shutdown(xsc)
	void *xsc;
{
	struct bge_softc *sc = (struct bge_softc *)xsc;

	bge_stop(sc);
	bge_reset(sc);
}

struct cfattach bge_ca = {
	sizeof(struct bge_softc), bge_probe, bge_attach
};

struct cfdriver bge_cd = {
	0, "bge", DV_IFNET
};
