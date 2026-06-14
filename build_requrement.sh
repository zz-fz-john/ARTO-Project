sudo xargs -a apt-requirements.txt apt-get install -y
cd ~
curl -O https://bootstrap.pypa.io/pip/2.7/get-pip.py
python2 get-pip.py --user "pip==20.3.4"
cd ~/ARTO
pip2 install -r requirements-py2.txt
pip3 install -r requirements-py3.txt