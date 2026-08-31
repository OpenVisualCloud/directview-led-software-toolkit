# Deployment Assumptions and Security Model

dvledtx transmits uncompressed video using the SMPTE ST 2110-20 standard. That transport, and
the PTP timing it depends on, carry no authentication, encryption or integrity protection. The
security of a deployment therefore rests entirely on the media segment and the TX host being
**air-gapped**, not on controls inside this application.

> **Every host in the deployment must be air-gapped.** This is a hard requirement, not a
> recommendation or a hardening suggestion. Threat modelling of the current code shows that
> two thirds of the identified attacks become reachable the moment the TX host has any in-band
> access path, and that the air gap is the *only* control standing between an adversary and the
> confidentiality of the content on the wire. Read
> [What "Air-Gapped" Means Here](#what-air-gapped-means-here) before deploying.

This document records that trust boundary, the assumptions it depends on, and the risk that
remains if those assumptions are broken.

## Table of Contents

- [What "Air-Gapped" Means Here](#what-air-gapped-means-here)
- [Trust Boundary and Topology](#trust-boundary-and-topology)
- [Deployment Assumptions](#deployment-assumptions)
- [Switch Hardening Requirements](#switch-hardening-requirements)
- [Content and Configuration Ingress](#content-and-configuration-ingress)
- [Security Properties Not Provided by This Toolkit](#security-properties-not-provided-by-this-toolkit)
- [Residual Risk and Defence in Depth](#residual-risk-and-defence-in-depth)
- [Commissioning Validation Checklist](#commissioning-validation-checklist)

## What "Air-Gapped" Means Here

Air-gapped means there is **no network path of any kind** between the media segment (or the TX
host) and any other network, and **no in-band administrative access to the TX host**. The host
is administered only from a physical keyboard, monitor and mouse inside the locked cabinet.

**These are required:**

- No uplink, trunk, routed interface or wireless interface anywhere on the media switch.
- No SSH daemon, no Telnet, no VNC/RDP, no remote-management agent and no remote syslog on the
  TX host. Any such service must be disabled and masked, not merely firewalled.
- No secondary "management" NIC on the TX host. Every NIC declared in `interfaces[]` is consumed
  by ST 2110-20 transmission onto the dedicated switch, and no other NIC may be cabled.
- The host BMC (IPMI / iDRAC / iLO / AMT) disabled in firmware, or its NIC physically
  disconnected. A BMC is a live network stack with independent access to the platform.
- No Wi-Fi, Bluetooth, cellular modem or USB-Ethernet adapter enabled on the TX host.

**None of the following counts as an air gap.** Each of them leaves the in-band access path
open, and each re-enables the attack paths listed in
[Residual Risk and Defence in Depth](#residual-risk-and-defence-in-depth):

| Not an air gap | Why it fails |
|----------------|--------------|
| A dedicated VLAN on a shared switch | A VLAN is a configuration, not a boundary. A misconfigured trunk, a VLAN-hopping attack or a switch reset places the attacker inside |
| A firewall or ACL between the media segment and the corporate network | A filtered path is still a path; the firewall becomes a single point of failure carrying the whole security model |
| A jump host or bastion with SSH into the TX host | Reintroduces in-band access to the TX host — the root of the majority of identified attacks |
| "Isolated", "restricted" or "segregated" network per site convention | These terms are routinely used for filtered or VLAN-separated networks. Do not accept them as evidence of an air gap |
| An enabled BMC on a "management-only" network | The BMC has independent, always-on access to the platform below the operating system |
| SSH left enabled but "only reachable from inside the cabinet" | An overlooked live switch port makes it reachable; the software cannot tell the difference |

## Trust Boundary and Topology

```
            ┌───────────────────── locked cabinet ──────────────────────┐
            │                                                           │
            │    TX host             Dedicated          Receivers       │
            │    (dvledtx)    ──▶    L2 switch   ──▶    (FPGA)     ──▶ LED wall
            │    all NICs = TX       media + PTP        customer-        │
            │    no SSH / BMC        VLAN only          designed         │
            │    physical KVM only                                       │
            └───────────────────────────────────────────────────────────┘
                                     ▲
                                     └── air gap:
                                         no uplink, no routed path, no
                                         in-band management of any host
```

| Element | Role | Position relative to the boundary |
|---------|------|-----------------------------------|
| **TX host** (dvledtx) | Decodes, crops and transmits ST 2110-20 streams | Inside the locked cabinet; administered by physical KVM only |
| **Dedicated L2 switch** | Carries media (ST 2110-20) and PTP traffic only | The air gap itself — no uplink to a corporate or building network |
| **Receivers** | Receive streams and drive the LED panels | Inside the cabinet; **customer-designed (typically FPGA-based) and out of scope for this toolkit** |
| **Administrator** | Configures and operates the TX host | Enters through the physical cabinet lock, at the console — never over a network |

**All NICs on the TX host are used for transmission.** `interfaces[]` may declare up to 8 NICs,
and every one of them is consumed by ST 2110-20 TX onto the dedicated switch. The TX host does
not bridge the media segment to any other network, and has no NIC reserved for management.

## Deployment Assumptions

The model above is only valid while all of the following hold. They are the responsibility of
the integrator, and should be verified at installation and after any change to the
installation — see the [Commissioning Validation Checklist](#commissioning-validation-checklist):

| # | Assumption | Why it matters |
|---|------------|----------------|
| 1 | The TX host, switch and receivers are installed in a **physically locked cabinet**, with keyed access restricted to authorised administrators | Physical access to the equipment is equivalent to full control over what is displayed on the wall |
| 2 | The TX host has **no in-band management access**: no SSH, Telnet, VNC or RDP daemon, no management NIC, no remote console, and the BMC disabled or disconnected. Administration is by **physical KVM inside the cabinet only** | In-band access to the host is the root of the majority of the attacks in the threat model — configuration tampering, log tampering, malicious media, PATH hijacking of log rotation and log-path traversal all require it. Removing it removes all of them at once |
| 3 | The switch is **dedicated to the media segment** and has **no uplink** to a corporate, building or guest network | An uplink extends the trust boundary to every network it reaches |
| 4 | **All unused switch ports are administratively disabled** and assigned to an unused blackhole VLAN | A live port reachable outside the cabinet — for example a patched wall socket believed to be disconnected — places an attacker directly inside the boundary |
| 5 | Media and PTP traffic run on a **dedicated VLAN**, with no DHCP server and no general-purpose hosts attached to it | Prevents an attached device from being addressed onto the media network, and keeps unrelated traffic off the transmission path |
| 6 | The switch is configured according to [Switch Hardening Requirements](#switch-hardening-requirements) | The switch is the air gap. Its configuration is the boundary, so it must fail closed |
| 7 | Configuration files and video content reach the host under the controls in [Content and Configuration Ingress](#content-and-configuration-ingress) | On an air-gapped host, removable media becomes the primary route for untrusted input |
| 8 | Administrator access to the host is **named and auditable** — no shared login, and physical cabinet/KVM access is logged | Actions taken at the console are otherwise unattributable; the toolkit does not sign or version its configuration |

## Switch Hardening Requirements

The dedicated switch *is* the air gap, so its configuration carries the boundary. Apply all of
the following and record the running configuration as part of the commissioning evidence.
Exact command syntax varies by vendor.

**Port control**

- **Disable every unused port** administratively (`shutdown`), and additionally place it in an
  unused blackhole VLAN so that re-enabling a port by mistake does not grant media access.
- **Enable port security** on every access port: 802.1X where the receivers support it,
  otherwise sticky MAC learning limited to one address per port.
- Set the port-security violation action to **shutdown** (err-disable), not `protect` or
  `restrict`, and disable auto-recovery so a violation requires a deliberate administrator
  action to clear.
- **Disable dynamic trunking** (DTP) and hard-set every port to access mode. Do not leave any
  port able to negotiate a trunk.
- Set the **native VLAN** of any trunk to an unused VLAN, and never to the media VLAN.

**Traffic control**

- **IGMP snooping enabled** with a **static querier** on the media VLAN, so multicast is
  forwarded only to ports that joined the group rather than flooded.
- **Storm control** on broadcast, multicast and unknown-unicast traffic, sized above the
  expected ST 2110-20 rate, to bound a flooding attack against the DPDK TX rings.
- **BPDU guard** and **root guard** on all access ports, so an attached device cannot influence
  the spanning tree.
- **DHCP snooping** and **dynamic ARP inspection** on the media VLAN, with no trusted DHCP port.
- **Disable CDP/LLDP** on access ports so topology information is not advertised to anything
  plugged into the cabinet.

**Management plane of the switch itself**

- Disable the switch's own remote management (SSH, Telnet, HTTP/HTTPS, SNMP) or restrict it to
  a serial console inside the cabinet. A switch reachable over the network is an uplink.
- Remove all default and vendor credentials; use named administrator accounts.
- Disable unused management services and any zero-touch/auto-provisioning feature, which will
  otherwise attempt to reach a provisioning server.
- Keep the switch configuration under version control, take a backup after every change, and
  compare the running configuration against that baseline during periodic review.

## Content and Configuration Ingress

Air-gapping does not remove the attacks that rely on malicious input — it changes how that
input arrives. Once the host has no network path, **removable media becomes the primary
ingress** for JSON configuration files and video source files, and the following attacks travel
with it: a crafted config aimed at the configuration parser, an oversized or deeply nested
config aimed at memory, a crafted media container aimed at the FFmpeg decoder, and substitution
of the video source with unauthorised content.

The integrator must therefore operate a controlled ingress procedure:

- Use **dedicated, controlled removable media** for the deployment. Do not use media that has
  been connected to an uncontrolled host.
- **Record a SHA-256 checksum** of every configuration file and every video source file at the
  point of authoring, on a trusted host, and verify it on the TX host before use.
- **Keep configuration files under version control** with a named author per change, so a
  configuration on the host can be traced back to who produced it.
- **Scan media files on a separate, non-production host** before transfer. The FFmpeg decoder
  processes untrusted containers and is the largest untrusted-input surface in the toolkit.
- **Restrict filesystem permissions** on the configuration directory, the video source
  directory and the log directory so that only the administrator account can write to them.
  The toolkit rejects symlinked configuration files, but it cannot defend a directory that
  unprivileged users can write to.
- **Retain the media and the checksum record** as commissioning evidence.

## Security Properties Not Provided by This Toolkit

Transmission is delegated to the
[Media Transport Library (MTL)](https://github.com/OpenVisualCloud/Media-Transport-Library) and
follows the SMPTE ST 2110 standards. dvledtx hands decoded frames to MTL and does not add
security controls above it — the security of the wire is offloaded entirely to the air gap.

State the following explicitly when assessing a deployment:

- **No authentication** of ST 2110-20 senders or receivers.
- **No encryption** of the video payload on the wire.
- **No integrity or replay protection** on the media path.
- **No authentication on the PTP path** — PTP is a broadcast protocol with no grandmaster
  validation.
- **No signing or versioning of the JSON configuration** — a configuration change made at the
  console cannot be attributed by the application.
- **No integrity verification of the video source file** — the toolkit transmits whatever file
  the configuration points at.
- **No tamper-evident logging** — log files can be edited or removed by anyone with write
  access to the log directory.

## Residual Risk and Defence in Depth

The air gap reduces exposure but does not eliminate it. If an adversary obtains Layer 2
access to the media VLAN — for example by connecting to an overlooked live switch port — or
reaches the host through removable media or the console, the following become feasible and are
**not** mitigated in software:

| Risk | Effect | Mitigation |
|------|--------|------------|
| Injection of a rogue ST 2110-20 stream | Unauthorised or offensive content displayed on the LED wall | Physically block access to the L2 media VLAN (locked cabinet, disabled unused ports, switch port security / 802.1X); operator visual detection of anomalous wall output |
| Rogue PTP grandmaster | TX pacing skew and stream disruption | Keep PTP on the air-gapped VLAN; leave PTP disabled (default TSC pacing) unless a trusted grandmaster is present |
| Traffic flooding on the media VLAN | Frame loss and visible artefacts on the wall | Dedicated switch with no other traffic; IGMP snooping and storm control |
| Passive capture of video | Loss of content confidentiality | **The air gap only** — no encryption is available on the ST 2110-20 path. This risk has no software mitigation whatsoever |
| Substituted video source or configuration file arriving on removable media | Unauthorised content on the wall, or streams redirected to an attacker-chosen destination | Controlled ingress and SHA-256 verification per [Content and Configuration Ingress](#content-and-configuration-ingress); version-controlled configuration |
| Log files edited or deleted to conceal activity | Loss of the only record of what was configured and displayed | Restrict write access to the log directory to the administrator account; export logs to controlled removable media as part of the maintenance procedure and retain them off-host |
| Configuration changed at the console with no attribution | Change cannot be traced to an individual | Named administrator accounts, logged physical cabinet/KVM access, and version-controlled configuration files |

**Detection and response are physical and visual.** There is no in-band alerting: an operator
observes anomalous output on the LED wall, and an administrator unlocks the cabinet to inspect,
disconnect or power-cycle the affected equipment. Because the host is air-gapped, log review is
also a physical activity — build it into the scheduled maintenance procedure, since nothing
will be noticed between visits.

> **Known accepted risk.** Because all authentication and encryption are absent by design of the
> transport, an adversary who defeats the air gap has no further software barrier. This is
> accepted for the current deployment profile and is pending validation by an
> adversarial-testing exercise. This document will be revisited if authentication or encryption
> become available in the underlying transport.

## Commissioning Validation Checklist

Work through this at installation, after any change to the installation, and at each periodic
review. Record the result and retain it as commissioning evidence. A failure against any item
means the security model in this document does not hold for the deployment.

**TX host**

- [ ] No listening network service on the host — verified with `ss -tulpn` at the console.
- [ ] SSH, Telnet, VNC, RDP and any remote-management agent are disabled **and masked**.
- [ ] No remote syslog or metrics forwarding is configured.
- [ ] Every NIC present is either declared in `interfaces[]` for ST 2110-20 TX or is
      administratively down and uncabled.
- [ ] Wi-Fi, Bluetooth and cellular interfaces are disabled in firmware or physically absent.
- [ ] The BMC (IPMI / iDRAC / iLO / AMT) is disabled in firmware, or its NIC is disconnected.
- [ ] Administrator accounts are named individually; no shared login is in use.
- [ ] The configuration, video source and log directories are writable only by the
      administrator account.

**Switch**

- [ ] No uplink, trunk, routed interface or wireless interface is present or configured.
- [ ] Every unused port is `shutdown` **and** assigned to an unused blackhole VLAN.
- [ ] Port security (802.1X or sticky MAC, one address per port) is active on every access port.
- [ ] Port-security violation action is `shutdown`, with auto-recovery disabled.
- [ ] Dynamic trunking (DTP) is disabled and every port is hard-set to access mode.
- [ ] IGMP snooping with a static querier is enabled on the media VLAN.
- [ ] Storm control, BPDU guard and root guard are enabled on access ports.
- [ ] DHCP snooping and dynamic ARP inspection are enabled on the media VLAN.
- [ ] CDP/LLDP are disabled on access ports.
- [ ] The switch's own remote management is disabled or console-only; default credentials
      removed; auto-provisioning disabled.
- [ ] The running configuration is backed up and matches the version-controlled baseline.

**Physical and procedural**

- [ ] Cabinet lock is fitted and functional; key holders are a named, current list.
- [ ] Physical/KVM access to the TX host is logged.
- [ ] Every patched wall socket and cable run into the cabinet is traced and accounted for.
- [ ] SHA-256 checksums are recorded for the deployed configuration files and video source
      files, and were verified on the host.
- [ ] Configuration files are under version control with a named author per change.
- [ ] Removable media used for ingress is dedicated to the deployment and controlled.
- [ ] A log-export and log-review step exists in the scheduled maintenance procedure.
- [ ] PTP is disabled unless a trusted grandmaster is present inside the boundary.
