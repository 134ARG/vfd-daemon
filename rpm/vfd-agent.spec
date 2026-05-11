Name:           vfd-agent
Version:        1.0.1
Release:        1%{?dist}
Summary:        Metric sender for vfd-daemon

License:        LicenseRef-Project
URL:            https://github.com/134arg/vfd-daemon
Source0:        vfd-daemon-%{version}.tar.gz

BuildArch:      noarch

Requires:       python3
Requires:       python3-psutil
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

BuildRequires:  systemd-rpm-macros

%description
vfd-agent collects local CPU, memory, temperature, network, GPU, uptime,
and failed-systemd-unit metrics, then streams them as newline-delimited JSON
to a vfd-daemon instance over TCP.

%prep
%autosetup -n vfd-daemon-%{version}

%build
# Python script; nothing to build.

%install
install -D -m 0755 agent/metric_agent.py %{buildroot}%{_bindir}/vfd-agent
install -D -m 0644 dist/vfd-agent.service %{buildroot}%{_unitdir}/vfd-agent.service
install -D -m 0644 dist/vfd-agent.conf %{buildroot}%{_sysconfdir}/vfd-agent/vfd-agent.conf

%post
%systemd_post vfd-agent.service

%preun
%systemd_preun vfd-agent.service

%postun
%systemd_postun_with_restart vfd-agent.service

%files
%doc README.md
%{_bindir}/vfd-agent
%{_unitdir}/vfd-agent.service
%dir %{_sysconfdir}/vfd-agent
%config(noreplace) %{_sysconfdir}/vfd-agent/vfd-agent.conf

%changelog
* Mon May 11 2026 134arg <xen134@outlook.com> - 1.0.1-1
- Add Fedora RPM definition for the standalone VFD metric agent.
