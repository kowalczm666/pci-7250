echo "drv --------"
doas kldload -v ./pci7250.ko
echo "--------"
kldstat
echo "--------"
cat /dev/din0 && echo ""
echo "--------"
for bit_device_out in /dev/dout0.*
do
    # ustaw
    echo $bit_device_out
    echo -n "1" > $bit_device_out
    sleep 0.5
    # sprawdz
    cat $bit_device_out && echo ""
    echo -n "0" > $bit_device_out
    sleep 0.5
    # sprawdz
    cat $bit_device_out && echo ""
done

dmesg | grep pci7250 | tail -n 40
echo "--------"
doas kldunload -v pci7250.ko
echo "fin --------"
