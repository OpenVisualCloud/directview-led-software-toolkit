# Security Policy
Intel is committed to rapidly addressing security vulnerabilities affecting our customers and providing clear guidance on the solution, impact, severity and mitigation. 

## Security Model

dvledtx follows a layered security model. The application validates and bounds the inputs it
owns — configuration parsing, log file destinations, decoder behaviour and resource limits — and
is covered by static analysis, fuzzing and dependency CVE tracking. Transport-level security is
a separate layer: SMPTE ST 2110-20 and its PTP timing do not define authentication, encryption
or integrity protection, which is a characteristic of the standards rather than of this
implementation, so the deployment architecture supplies that layer.

For that reason we recommend deploying on an **air-gapped** media segment: no network path to
any other network, and no in-band administrative access to the TX host (no SSH, no management
NIC, no remote console, BMC disabled). A VLAN on a shared switch, a firewall or a jump host does
not meet this definition.

**In scope:** the dvledtx transmitter application, its JSON configuration parsing, and its use
of FFmpeg and the Media Transport Library.

**Out of scope:** the receivers (customer-designed, typically FPGA-based, and not delivered with
this toolkit), the LED panels, and the physical and network controls of the installation site.

The trust boundary, the deployment assumptions, the switch hardening recommendations, the
controls the toolkit implements, and the residual risk are documented in
[Deployment Assumptions and Security Model](docs/assumptions.md).

## Reporting a Vulnerability
Please report any security vulnerabilities in this project [utilizing the guidelines here](https://www.intel.com/content/www/us/en/security-center/vulnerability-handling-guidelines.html).

