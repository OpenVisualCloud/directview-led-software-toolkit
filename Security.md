# Security Policy
Intel is committed to rapidly addressing security vulnerabilities affecting our customers and providing clear guidance on the solution, impact, severity and mitigation. 

## Security Model

dvledtx is designed for deployment on an **air-gapped** media segment: no network path to any
other network, and no in-band administrative access to the TX host (no SSH, no management NIC,
no remote console, BMC disabled). The transport (SMPTE ST 2110-20) and its PTP timing provide
no authentication, encryption or integrity protection; security is delegated to MTL/ST 2110 and
to that air gap. A VLAN, a firewall or a jump host does not satisfy this requirement.

**In scope:** the dvledtx transmitter application, its JSON configuration parsing, and its use
of FFmpeg and the Media Transport Library.

**Out of scope:** the receivers (customer-designed, typically FPGA-based, and not delivered with
this toolkit), the LED panels, and the physical and network controls of the installation site.

The trust boundary, the deployment assumptions this model depends on, the switch hardening
requirements, and the residual risk if those assumptions are broken are documented in
[Deployment Assumptions and Security Model](docs/assumptions.md).

## Reporting a Vulnerability
Please report any security vulnerabilities in this project [utilizing the guidelines here](https://www.intel.com/content/www/us/en/security-center/vulnerability-handling-guidelines.html).

