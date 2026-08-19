# Deployment Assumptions and Security Model

dvledtx transmits uncompressed video using the SMPTE ST 2110-20 standard. That transport, and
the PTP timing it depends on, carry no authentication, encryption or integrity protection. The
security of a deployment therefore rests on the **physical and Layer 2 isolation of the media
segment**, not on controls inside this application.

This document records that trust boundary, the assumptions it depends on, and the risk that
remains if those assumptions are broken.

## Table of Contents

- [Trust Boundary and Topology](#trust-boundary-and-topology)
- [Deployment Assumptions](#deployment-assumptions)
- [Security Properties Not Provided by This Toolkit](#security-properties-not-provided-by-this-toolkit)
- [Residual Risk and Defence in Depth](#residual-risk-and-defence-in-depth)

## Trust Boundary and Topology

```
            ┌───────────────────── locked cabinet ──────────────────────┐
            │                                                           │
            │    TX host             Dedicated          Receivers       │
            │    (dvledtx)    ──▶    L2 switch   ──▶    (FPGA)     ──▶ LED wall
            │    all NICs = TX       media + PTP        customer-        │
            │                        VLAN only          designed         │
            └───────────────────────────────────────────────────────────┘
                                     ▲
                                     └── isolation boundary:
                                         no uplink to any other network
```

| Element | Role | Position relative to the boundary |
|---------|------|-----------------------------------|
| **TX host** (dvledtx) | Decodes, crops and transmits ST 2110-20 streams | Inside the locked cabinet |
| **Dedicated L2 switch** | Carries media (ST 2110-20) and PTP traffic only | The isolation boundary itself — no uplink to a corporate or building network |
| **Receivers** | Receive streams and drive the LED panels | Inside the cabinet; **customer-designed (typically FPGA-based) and out of scope for this toolkit** |
| **Administrator** | Configures and operates the TX host | Enters through the physical cabinet lock |

**All NICs on the TX host are used for transmission.** `interfaces[]` may declare up to 8 NICs,
and every one of them is consumed by ST 2110-20 TX onto the dedicated switch. The TX host does
not bridge the media segment to any other network.

## Deployment Assumptions

The model above is only valid while all of the following hold. They are the responsibility of
the integrator, and should be verified at installation and after any change to the
installation:

| # | Assumption | Why it matters |
|---|------------|----------------|
| 1 | The TX host, switch and receivers are installed in a **physically locked cabinet**, with keyed access restricted to authorised administrators | Physical access to the equipment is equivalent to full control over what is displayed on the wall |
| 2 | The switch is **dedicated to the media segment** and has **no uplink** to a corporate, building or guest network | An uplink extends the trust boundary to every network it reaches |
| 3 | **All unused switch ports are administratively disabled** | A live port reachable outside the cabinet — for example a patched wall socket believed to be disconnected — places an attacker directly inside the boundary |
| 4 | Media and PTP traffic run on a **dedicated VLAN**, with no DHCP server and no general-purpose hosts attached to it | Prevents an attached device from being addressed onto the media network, and keeps unrelated traffic off the transmission path |

## Security Properties Not Provided by This Toolkit

Transmission is delegated to the
[Media Transport Library (MTL)](https://github.com/OpenVisualCloud/Media-Transport-Library) and
follows the SMPTE ST 2110 standards. dvledtx hands decoded frames to MTL and does not add
security controls above it — the security of the wire is offloaded to physical and Layer 2
isolation.

State the following explicitly when assessing a deployment:

- **No authentication** of ST 2110-20 senders or receivers.
- **No encryption** of the video payload on the wire.
- **No integrity or replay protection** on the media path.
- **No authentication on the PTP path** — PTP is a broadcast protocol with no grandmaster
  validation.

## Residual Risk and Defence in Depth

Physical isolation reduces exposure but does not eliminate it. If an adversary obtains Layer 2
access to the media VLAN — for example by connecting to an overlooked live switch port — the
following become feasible and are **not** mitigated in software:

| Risk | Effect | Mitigation |
|------|--------|------------|
| Injection of a rogue ST 2110-20 stream | Unauthorised or offensive content displayed on the LED wall | Physically block access to the L2 media VLAN (locked cabinet, disabled unused ports, switch port security / 802.1X); operator visual detection of anomalous wall output |
| Rogue PTP grandmaster | TX pacing skew and stream disruption | Keep PTP on the isolated VLAN; leave PTP disabled (default TSC pacing) unless a trusted grandmaster is present |
| Traffic flooding on the media VLAN | Frame loss and visible artefacts on the wall | Dedicated switch with no other traffic; IGMP snooping |
| Passive capture of video | Loss of content confidentiality | Physical isolation only — no encryption is available on the ST 2110-20 path |

**Detection and response are physical and visual.** There is no in-band alerting: an operator
observes anomalous output on the LED wall, and an administrator unlocks the cabinet to inspect,
disconnect or power-cycle the affected equipment.

> **Known accepted risk.** Because all authentication and encryption are absent by design of the
> transport, an adversary who defeats the physical and L2 isolation has no further software
> barrier. This is accepted for the current deployment profile and is pending validation by an
> adversarial-testing exercise. This document will be revisited if authentication or encryption
> become available in the underlying transport.
