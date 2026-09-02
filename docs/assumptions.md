# Deployment Assumptions and Security Model

dvledtx is designed for a layered security model. The application implements its own controls
at every input it owns — configuration parsing is bounds-checked and validated, symlinked
configuration files are rejected, log file paths are restricted, decoder and resource limits
bound untrusted media, and dependencies are pinned and tracked for CVEs. Those controls are
listed in [Security Controls Implemented in dvledtx](#security-controls-implemented-in-dvledtx).

What the application cannot supply is transport-level security. dvledtx transmits uncompressed
video using SMPTE ST 2110-20, and neither that standard nor the PTP timing it depends on
defines authentication, encryption or integrity protection on the wire. This is a property of
the standards, common to all ST 2110 equipment, and it is why the deployment architecture forms
the outer layer of the model: the network design supplies the protection that the transport
itself does not define.

> **We recommend that every host in the deployment is air-gapped.** This is our recommended
> deployment model for a secure installation. Threat modelling of the current release shows
> that around two thirds of the identified attack paths depend on the TX host having an in-band
> access path, and that on an air-gapped segment the remaining paths require physical access to
> the cabinet. Because "isolated" and "restricted" are used loosely across the industry, we set
> out precisely what we mean in
> [What "Air-Gapped" Means Here](#what-air-gapped-means-here). Please read it before deploying.

This document records the trust boundary, the deployment assumptions the model depends on, the
controls the toolkit provides, and the residual risk that remains if those assumptions are not
met.

## Table of Contents

- [What "Air-Gapped" Means Here](#what-air-gapped-means-here)
- [Trust Boundary and Topology](#trust-boundary-and-topology)
- [Deployment Assumptions](#deployment-assumptions)
- [Switch Hardening Recommendations](#switch-hardening-recommendations)
- [Content and Configuration Ingress](#content-and-configuration-ingress)
- [Security Controls Implemented in dvledtx](#security-controls-implemented-in-dvledtx)
- [Properties Supplied by the Deployment Rather Than the Toolkit](#properties-supplied-by-the-deployment-rather-than-the-toolkit)
- [Residual Risk and Defence in Depth](#residual-risk-and-defence-in-depth)
- [Commissioning Validation Checklist](#commissioning-validation-checklist)

## What "Air-Gapped" Means Here

Air-gapped means there is **no network path of any kind** between the media segment (or the TX
host) and any other network, and **no in-band administrative access to the TX host**. The host
is administered from a physical keyboard, monitor and mouse inside the locked cabinet.

**A deployment meets this definition when:**

- There is no uplink, trunk, routed interface or wireless interface anywhere on the media
  switch.
- The TX host runs no SSH daemon, Telnet, VNC/RDP, remote-management agent or remote syslog.
  Where such a service exists it is disabled and masked rather than firewalled.
- The TX host has no secondary "management" NIC. Every NIC declared in `interfaces[]` is
  consumed by ST 2110-20 transmission onto the dedicated switch, and no other NIC is cabled.
- The host BMC (IPMI / iDRAC / iLO / AMT) is disabled in firmware, or its NIC is physically
  disconnected. A BMC is a live network stack with independent access to the platform.
- Wi-Fi, Bluetooth, cellular modems and USB-Ethernet adapters are disabled on the TX host.

**The following arrangements do not meet this definition.** Each leaves an in-band access path
open, which re-enables the attack paths described in
[Residual Risk and Defence in Depth](#residual-risk-and-defence-in-depth). We list them because
they are frequently presented as equivalent:

| Arrangement | Why it does not meet the definition |
|-------------|-------------------------------------|
| A dedicated VLAN on a shared switch | A VLAN is a configuration, not a boundary. A misconfigured trunk, a VLAN-hopping attack or a switch reset places an attacker inside |
| A firewall or ACL between the media segment and the corporate network | A filtered path is still a path, and the firewall becomes a single point of failure carrying the whole model |
| A jump host or bastion with SSH into the TX host | Reintroduces in-band access to the TX host, which the threat model identifies as the largest single contributor of attack paths |
| "Isolated", "restricted" or "segregated" network per site convention | These terms are commonly applied to filtered or VLAN-separated networks. Confirm the actual topology rather than relying on the label |
| An enabled BMC on a "management-only" network | The BMC has independent, always-on access to the platform below the operating system |
| SSH enabled but "only reachable from inside the cabinet" | An overlooked live switch port makes it reachable, and the software cannot distinguish the two cases |

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

The model above depends on the following holding true. They are the responsibility of the
integrator, and should be verified at installation and after any change to the installation —
see the [Commissioning Validation Checklist](#commissioning-validation-checklist):

| # | Assumption | Why it matters |
|---|------------|----------------|
| 1 | The TX host, switch and receivers are installed in a **physically locked cabinet**, with keyed access restricted to authorised administrators | Physical access to the equipment is equivalent to full control over what is displayed on the wall |
| 2 | The TX host has **no in-band management access**: no SSH, Telnet, VNC or RDP daemon, no management NIC, no remote console, and the BMC disabled or disconnected. Administration is by **physical KVM inside the cabinet only** | In-band access to the host is the largest single contributor of attack paths in the threat model, since it is the precondition for configuration tampering, log tampering, supplying malicious media and influencing the log rotation environment. Removing it addresses all of them together |
| 3 | The switch is **dedicated to the media segment** and has **no uplink** to a corporate, building or guest network | An uplink extends the trust boundary to every network it reaches |
| 4 | **All unused switch ports are administratively disabled** and assigned to an unused blackhole VLAN | A live port reachable outside the cabinet — for example a patched wall socket believed to be disconnected — places an attacker directly inside the boundary |
| 5 | Media and PTP traffic run on a **dedicated VLAN**, with no DHCP server and no general-purpose hosts attached to it | Prevents an attached device from being addressed onto the media network, and keeps unrelated traffic off the transmission path |
| 6 | The switch is configured according to [Switch Hardening Recommendations](#switch-hardening-recommendations) | The switch carries the boundary, so its configuration should fail closed |
| 7 | Configuration files and video content reach the host under the controls in [Content and Configuration Ingress](#content-and-configuration-ingress) | On an air-gapped host, removable media becomes the primary route for untrusted input |
| 8 | Administrator access to the host is **named and auditable** — no shared login, and physical cabinet/KVM access is logged | Actions taken at the console are otherwise unattributable, and the current release does not sign or version its configuration |

## Switch Hardening Recommendations

The dedicated switch carries the boundary, so its configuration matters as much as the topology.
We recommend applying all of the following and recording the running configuration as part of
the commissioning evidence. Exact command syntax varies by vendor.

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

## Security Controls Implemented in dvledtx

The application applies controls at each interface it owns. These are present in the current
release and are covered by the test suite:

**Configuration input**

- The JSON parser is bounds-checked throughout and operates on a fixed, validated schema.
- Crop rectangles, scaling parameters, UDP ports, payload types and IP addresses are validated
  at parse time; out-of-range values, privileged UDP ports and unsupported pixel formats are
  rejected before any resource is allocated.
- Symlinked configuration files are rejected, so a configuration path cannot be redirected to
  another file on the host.
- The log file destination is restricted to `/var/log/` or the working directory, so a
  configuration cannot be used to write to an arbitrary location.

**Media input**

- A decoder watchdog bounds decode attempts per frame, so a malformed or truncated container
  cannot stall transmission indefinitely.
- Raw video source files are size-capped before being read into memory.
- FFmpeg's own logging is set to error level so that internal paths and addresses are not
  written into the application log.

**Runtime behaviour**

- Signal handling is async-signal-safe, and shutdown performs a final barrier synchronisation so
  that a stop request cannot leave transmission threads deadlocked.
- Session resources are allocated only after the full configuration has been validated.

**Development lifecycle**

- Dependencies (FFmpeg, MTL, DPDK) are pinned to audited versions, with CVE tracking through
  Dependabot.
- Continuous analysis covers Coverity, CodeQL, cppcheck, Trivy, ShellCheck and OpenSSF
  Scorecard, with binary hardening verified by checksec.
- The configuration reader is fuzzed with both AFL and libFuzzer against a maintained corpus.
- Unit and smoke tests run on every pull request.

## Properties Supplied by the Deployment Rather Than the Toolkit

Some security properties are outside what the application can provide, and are supplied by the
deployment architecture instead. State these explicitly when assessing an installation, so that
the responsibility for each is clear.

**Inherent to the ST 2110 transport.** These apply to all ST 2110-20 equipment, not only to
dvledtx. Transmission is delegated to the
[Media Transport Library (MTL)](https://github.com/OpenVisualCloud/Media-Transport-Library),
which implements the standards as published:

| Property | Where it comes from instead |
|----------|-----------------------------|
| Authentication of ST 2110-20 senders and receivers | The standard defines no sender or receiver authentication. Provided by controlling which devices can attach to the media segment |
| Encryption of the video payload | The standard carries the payload in clear RTP. Provided by the physical security of the segment |
| Integrity and replay protection on the media path | Not defined by the standard. Provided by the physical security of the segment |
| Authentication of the PTP timing source | IEEE 1588 provides no grandmaster validation. Provided by keeping PTP inside the boundary, and by leaving PTP disabled (default TSC pacing) unless a trusted grandmaster is present |

**Currently supplied by procedure, and candidates for future releases.** These are provided by
the operational controls in [Content and Configuration Ingress](#content-and-configuration-ingress)
rather than by the application today:

| Property | Current provision |
|----------|-------------------|
| Attribution of configuration changes | Named administrator accounts and version-controlled configuration files. Signing and versioning of the JSON configuration is under consideration for a future release |
| Integrity of the video source file | SHA-256 checksums recorded at authoring and verified before use. In-application verification at startup is under consideration for a future release |
| Tamper-evident logging | Filesystem permissions on the log directory, and scheduled export of logs to controlled media for off-host retention |

## Residual Risk and Defence in Depth

The air gap reduces exposure but does not eliminate it. If an adversary obtains Layer 2 access
to the media VLAN — for example by connecting to an overlooked live switch port — or reaches
the host through removable media or the console, the following remain possible. They sit
outside what the application can address, and are handled by the physical and procedural
controls shown:

| Risk | Effect | Mitigation |
|------|--------|------------|
| Injection of a rogue ST 2110-20 stream | Unauthorised or offensive content displayed on the LED wall | Physically block access to the L2 media VLAN (locked cabinet, disabled unused ports, switch port security / 802.1X); operator visual detection of anomalous wall output |
| Rogue PTP grandmaster | TX pacing skew and stream disruption | Keep PTP on the air-gapped VLAN; leave PTP disabled (default TSC pacing) unless a trusted grandmaster is present |
| Traffic flooding on the media VLAN | Frame loss and visible artefacts on the wall | Dedicated switch with no other traffic; IGMP snooping and storm control |
| Passive capture of video | Loss of content confidentiality | The deployment architecture, since the ST 2110-20 path defines no encryption. Confidentiality of the payload depends on controlling physical access to the segment |
| Substituted video source or configuration file arriving on removable media | Unauthorised content on the wall, or streams redirected to an attacker-chosen destination | Controlled ingress and SHA-256 verification per [Content and Configuration Ingress](#content-and-configuration-ingress); version-controlled configuration |
| Log files edited or deleted to conceal activity | Loss of the only record of what was configured and displayed | Restrict write access to the log directory to the administrator account; export logs to controlled removable media as part of the maintenance procedure and retain them off-host |
| Configuration changed at the console with no attribution | Change cannot be traced to an individual | Named administrator accounts, logged physical cabinet/KVM access, and version-controlled configuration files |

**Detection and response are physical and visual.** There is no in-band alerting: an operator
observes anomalous output on the LED wall, and an administrator unlocks the cabinet to inspect,
disconnect or power-cycle the affected equipment. Because the host is air-gapped, log review is
also a physical activity — build it into the scheduled maintenance procedure, since nothing
will be noticed between visits.

> ### Accepted risk for the current deployment profile
>
> Because the ST 2110-20 transport defines no authentication or encryption, an adversary who
> gains Layer 2 access to the media segment can observe or inject media traffic, and the
> application has no means of detecting or preventing it. This is a known characteristic of the
> transport rather than a defect in the toolkit.
>
> **What has been accepted.** For the deployment profile described in this document — an
> air-gapped media segment inside a locked cabinet, with a dedicated switch and no in-band
> management access — the project has assessed this exposure as acceptable, on the basis that
> reaching the segment requires physical access to the cabinet or to cabling within the secured
> installation area. Deployments that cannot meet those conditions should treat the exposure as
> unmitigated and reassess it against their own risk appetite.
>
> **What is still outstanding.** The acceptance is provisional pending an adversarial-testing
> exercise, in which a tester is given Layer 2 access to a representative media segment and
> attempts stream injection, passive capture, PTP disruption and traffic flooding against a
> hardened switch configuration. The purpose is to confirm that the switch hardening in this
> document behaves as intended, and to establish how quickly anomalous wall output is noticed
> in practice, since detection is visual. The results will be recorded here.
>
> **What would change this position.** If authentication or encryption become available in the
> underlying transport — for example through adoption of a secured ST 2110 profile in MTL — the
> project will re-evaluate and this document will be revised.

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
