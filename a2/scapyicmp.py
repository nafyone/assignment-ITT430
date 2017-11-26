#! /usr/bin/env python
from scapy.all import send, IP, ICMP

send(IP(src="192.168.232.128",dst="192.168.17.128")/ICMP()/"Hello World")
