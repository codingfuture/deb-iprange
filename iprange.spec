Summary:	Manage ip ranges
Name:		iprange
Version:	1.0.2
Release:	%{?release_suffix}%{?dist}
License:	GPLv2+
URL:		http://firehol.org
Source:		%{name}-1.0.2.tar.bz2

BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

%description
Manage ip ranges.

%prep
%setup -q -n %{name}-1.0.2

%build
%configure \
	--docdir="%{_docdir}/%{name}-%{version}" \
	%{?conf}
make %{?_smp_mflags}

%install
rm -rf "%{buildroot}"
make %{?_smp_mflags} install DESTDIR="%{buildroot}"

%files
%{_sbindir}/iprange

%changelog
* Sat Nov 28 2015 Phil Whineray <phil@firehol.org> - 1.0.2-1
- Bugfix release

* Sat Nov 28 2015 Phil Whineray <phil@firehol.org> - 1.0.1-1
- Release new version

* Sun Nov 15 2015 Alon Bar-Lev <alonbl@redhat.com> - 1.0.0-1
- Initial add.
