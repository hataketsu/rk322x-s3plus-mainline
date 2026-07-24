#!/usr/bin/env bash
# Run a command (or pipe a script) on the target box over SSH.
#
#   BOX_HOST=192.168.1.94 ./scripts/rsh.sh 'uname -a'
#   BOX_HOST=192.168.1.94 ./scripts/rsh.sh < some-script.sh
#
# Auth: uses your ssh key if set up (recommended — run scripts/setup-box-key.sh once).
# Falls back to $BOX_PASS via sshpass/expect if provided (dev convenience only).
set -euo pipefail
HOST=${BOX_HOST:?set BOX_HOST=<ip>}
USER=${BOX_USER:-root}
CMD=${*:-}

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15)

run_key()  { ssh "${SSH_OPTS[@]}" "$USER@$HOST" ${CMD:+"$CMD"}; }
run_pass() {
  expect -c "
    set timeout 300
    spawn ssh ${SSH_OPTS[*]} $USER@$HOST ${CMD:+bash -c {$CMD}}
    expect { -re {[Pp]assword:} { send \"$BOX_PASS\r\" } timeout { exit 2 } }
    expect eof
  "
}

if [[ -n "${BOX_PASS:-}" ]]; then run_pass; else run_key; fi
