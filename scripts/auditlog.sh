#!/bin/bash

echo "Killing auditd..."
sudo killall -9 auditd

echo "Copy audit log..."
sudo ausearch -r > audit_ori.log
#sudo mv /var/log/audit/audit.log ./audit_ori.log

echo "Sort audit log..."
auditlog_sort ./audit_ori.log > audit.log

echo "audit.log is ready..."

