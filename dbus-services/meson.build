dep = dependency('dbus-1')

org_enlightenment_ethumb_service_config = configuration_data()

org_enlightenment_ethumb_service_config.set('prefix', dir_prefix)

systemd_dbus_prefix = '# '
if get_option('systemd') == true
  systemd_dbus_prefix = ''
endif
org_enlightenment_ethumb_service_config.set('systemd_dbus_prefix',
                                            systemd_dbus_prefix)

# On Haiku dbus lives in its own read-only package directory, so install the
# service file under our own prefix instead.
if sys_haiku
  dbus_services_dir = join_paths(dir_data, 'dbus-1', 'services')
else
  dbus_services_dir = dep.get_variable(pkgconfig : 'session_bus_services_dir')
endif

configure_file(
  input: 'org.enlightenment.Ethumb.service.in',
  output: 'org.enlightenment.Ethumb.service',
  configuration: org_enlightenment_ethumb_service_config,
  install_dir: dbus_services_dir)
