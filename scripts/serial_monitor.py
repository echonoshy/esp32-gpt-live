"""USB monitor: type connect, interrupt or stop. Ctrl-C exits."""
import argparse
import os
import select
import sys
import termios
import time
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--port',default='/dev/cu.usbmodem1101')
p.add_argument('--log',default='artifacts/device-live.log')
args=p.parse_args()
Path(args.log).parent.mkdir(parents=True,exist_ok=True)
fd=os.open(args.port,os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
original=termios.tcgetattr(fd)
attrs=termios.tcgetattr(fd)
attrs[0]=0;attrs[1]=0;attrs[3]=0
attrs[2]=(attrs[2]|termios.CLOCAL|termios.CREAD)&~termios.HUPCL
attrs[4]=attrs[5]=termios.B115200
termios.tcsetattr(fd,termios.TCSANOW,attrs)
print('USB monitor connected; commands: connect / interrupt / stop',flush=True)
try:
    with open(args.log,'a') as log:
        while True:
            readable,_,_=select.select([fd,sys.stdin],[],[],1)
            if fd in readable:
                data=os.read(fd,8192).decode('utf-8','replace')
                if data: print(data,end='',flush=True);log.write(data);log.flush()
            if sys.stdin in readable:
                line=sys.stdin.readline()
                if not line:break
                os.write(fd,line.encode())
except KeyboardInterrupt:
    pass
finally:
    termios.tcsetattr(fd,termios.TCSANOW,original)
    os.close(fd)
