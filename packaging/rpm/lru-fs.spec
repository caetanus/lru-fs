Name:           lru-fs
Version:        0.1.0
Release:        1%{?dist}
Summary:        Scratch filesystem: recently used files in RAM, the rest on disk

License:        MIT
URL:            https://github.com/caetanus/lru-fs
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(fuse3)
BuildRequires:  systemd-rpm-macros
Requires:       fuse3

%description
lru-fs is a FUSE filesystem for scratch space (AI agents, builds, test clones,
temporary downloads). Files live as plain files in a RAM tier (tmpfs) and a
disk tier: recently used ones stay in RAM up to a budget, the rest move to
disk, files untouched for a few days are deleted, and a size cap or minimum
free space trims the least recently used first.

Ships a systemd user unit that mounts ~/scratch:
  systemctl --user enable --now lru-fs

%prep
%autosetup

%build
%make_build CFLAGS="%{optflags}" LDFLAGS="%{build_ldflags}"

%install
%make_install PREFIX=%{_prefix} USERUNITDIR=%{_userunitdir}

%files
%license LICENSE
%doc README.md
%{_bindir}/lru-fs
%{_userunitdir}/lru-fs.service

%changelog
* Thu Nov 19 2026 Marcelo A Caetano <rockristao@gmail.com> - 0.1.0-1
- Initial release
