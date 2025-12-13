Comprehensive instructions for installing and running xahaud are available on the [https://Xahau.Network](https://xahau.network/docs/infrastructure/installing-xahaud) documentation website.

## Create the Runtime Environment
xahaud can be [built from source](../../BUILD.md) or installed using the binary files available from [https://build.xahau.tech](https://build.xahau.tech/). After obtaining a working xahaud binary, users will need to provide a suitable runtime environment. The following setup can be used for Linux or Docker environments.

1. Create or download two configuration files: the main xahaud.cfg configuration file and a second validators-xahau.txt file defining which validators or UNL list publishers are trusted. The default location for these files in this xahaud repository is `cfg/`.
2. Provide a directory structure that is congruent with the contents of xahaud.cfg. This will include a location for logfiles, such as `/var/log/xahaud/`, as well as database files, `/opt/xahaud/db/`. Configuration files are, by default, sourced from `/etc/xahaud/`. It is possible to provide a symbolic link, if users wish to store configuration files elsewhere.
3. If desired, created a xahaud user and group, and change ownership of the binary and directories. Servers used for validating nodes should use the most restrictive permissions possible for `xahaud.cfg`, as the validation token is stored therein.
4. If desired, create a systemd service file: `/etc/systemd/system/xahaud.service`, enabling xahaud to run as a daemon. Alternately, run: `/path/to/binary/xahaud --conf=/path/to/xahaud.cfg`.

## Example systemd Service File
```
[Unit]
Description=Xahaud Daemon
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/path/to/xahaud --silent --conf /path/to/xahaud.cfg
Restart=on-failure
User=xahaud
Group=xahaud
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

After the systemd service file is installed, it must be loaded with: `systemctl daemon-reload`. xahaud can then be enabled: `systemctl enable --now xahaud`.
