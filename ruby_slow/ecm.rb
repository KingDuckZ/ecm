#!/usr/bin/env ruby

# ECM

#LUTs used for computing ECC/EDC
$ecc_f_lut = Array.new(256) #ecc_uint8
$ecc_b_lut = Array.new(256) #ecc_uint8
$edc_lut = Array.new(256) #ecc_uint32

$mycounter_analyze = 0
$mycounter_encode = 0
$mycounter_total = 0

def banner()
	$stderr.puts "ECM - Encoder for Error Code Modeler format v1.0"
	$stderr.puts "Copyright (C) 2002 Neill Corlett"
end

# Init routine
def eccedc_init()
	edc = 0
	0.upto(255) do |i|
		j = (i << 1) ^ (i & 0x80 ? 0x11D : 0)
		$ecc_f_lut[i] = j
		$ecc_b_lut[i ^ j] = i
		edc = i
		for j in 0..8
			edc = (edc >> 1) ^ (edc & 1 ? 0xD8018001 : 0)
		end
		$edc_lut[i] = edc;
	end
	nil
end

#***************************************************************************/
#*
#* Compute EDC for a block
#/
def edc_computeblock(edc, src)
	(src.length - 1).downto(0) do |z|
		edc = (edc >> 8) ^ $edc_lut[(edc ^ src[z]) & 0xFF]
	end
	return edc
end

#***************************************************************************/
#*
#* Compute ECC for a block (can do either P or Q)
#/
def ecc_computeblock(src, major_count, minor_count, major_mult, minor_inc, dest)
	size = major_count * minor_count
	0.upto(major_count - 1) do |major|
		index = (major >> 1) * major_mult + (major & 1)
		ecc_a = 0
		ecc_b = 0
		0.upto(minor_count - 1) do |minor|
			temp = src[index]
			index += minor_inc
			index -= size if index >= size
			ecc_a ^= temp
			ecc_b ^= temp
			ecc_a = $ecc_f_lut[ecc_a];
		end
		ecc_a = $ecc_b_lut[$ecc_f_lut[ecc_a] ^ ecc_b]
		return 0 if dest[major] != ecc_a
		return 0 if dest[major + major_count] != (ecc_a ^ ecc_b)
	end
	return 1
end

#*
#* Generate ECC P and Q codes for a block
#/
def ecc_generate(sector, zeroaddress, dest)
	address = Array.new(4)
	#Save the address and zero it out
	if zeroaddress != 0 then
		address[0..3] = sector[12..15]
		sector[12..15] = [0, 0, 0, 0]
	end
	#Compute ECC P code
	if 0 == ecc_computeblock(sector + 0xC, 86, 24,  2, 86, dest + 0x81C - 0x81C) then
		sector[12..15] = address[0..3] if zeroaddress != 0
		return 0
	end

	#Compute ECC Q code
	r = ecc_computeblock(sector + 0xC, 52, 43, 86, 88, dest + 0x8C8 - 0x81C)
	#Restore the address
	sector[12..15] = address[0..3] if zeroaddress != 0
	return r;
end

#***************************************************************************/

#*
#* sector types:
#* 00 - literal bytes
#* 01 - 2352 mode 1         predict sync, mode, reserved, edc, ecc
#* 02 - 2336 mode 2 form 1  predict redundant flags, edc, ecc
#* 03 - 2336 mode 2 form 2  predict redundant flags, edc
#/

def check_type(sector_neg, sector, canbetype1)
	canbetype2 = true
	canbetype3 = true
	#Check for mode 1
	if canbetype1 then
		if sector[0x00] != 0x00 || sector[0x01] != 0xFF ||
			sector[0x02] != 0xFF || sector[0x03] != 0xFF ||
			sector[0x04] != 0xFF || sector[0x05] != 0xFF ||
			sector[0x06] != 0xFF || sector[0x07] != 0xFF ||
			sector[0x08] != 0xFF || sector[0x09] != 0xFF ||
			sector[0x0A] != 0xFF || sector[0x0B] != 0x00 ||
			sector[0x0F] != 0x01 || sector[0x814] != 0x00 ||
			sector[0x815] != 0x00 || sector[0x816] != 0x00 ||
			sector[0x817] != 0x00 || sector[0x818] != 0x00 ||
			sector[0x819] != 0x00 || sector[0x81A] != 0x00 ||
			sector[0x81B] != 0x00 then

			canbetype1 = false
		end
	end

	#Check for mode 2
	if sector[0x0] != sector[0x4] || sector[0x1] != sector[0x5] ||
		sector[0x2] != sector[0x6] || sector[0x3] != sector[0x7] then

		canbetype2 = false
		canbetype3 = false
		return 0 unless canbetype1
	end

	#Check EDC
	if canbetype2 then
		myedc = edc_computeblock(0, sector[0x00..0x807])
		if sector[0x808] != ((myedc >>  0) & 0xFF) ||
			sector[0x809] != ((myedc >>  8) & 0xFF) ||
			sector[0x80A] != ((myedc >> 16) & 0xFF) ||
			sector[0x80B] != ((myedc >> 24) & 0xFF) then

			canbetype2 = false
		end
	end
	if canbetype1 then
		myedc = edc_computeblock(myedc, sector[0x808..(0x808+8-1)])
		if sector[0x810] != ((myedc >>  0) & 0xFF) ||
			sector[0x811] != ((myedc >>  8) & 0xFF) ||
			sector[0x812] != ((myedc >> 16) & 0xFF) ||
			sector[0x813] != ((myedc >> 24) & 0xFF) then

			canbetype1 = false
		end
	end
	if canbetype3 then
		myedc = edc_computeblock(myedc, sector[0x810..(0x010+0x10C-1)])
		if sector[0x91C] != ((myedc >>  0) & 0xFF) ||
			sector[0x91D] != ((myedc >>  8) & 0xFF) ||
			sector[0x91E] != ((myedc >> 16) & 0xFF) ||
			sector[0x91F] != ((myedc >> 24) & 0xFF) then

			canbetype3 = false
		end
	end

	#Check ECC
	if canbetype1 then
		canbetype1 = false if 0 == ecc_generate(sector, 0, sector[0x81C..-1])
	end
	if canbetype2 then
		canbetype2 = false if 0 == ecc_generate(sector_neg, 1, sector[0x80C..-1])
	end
	return 1 if canbetype1
	return 2 if canbetype2
	return 3 if canbetype3
	return 0
end

#***************************************************************************/
#*
#* Encode a type/count combo
#/
def write_type_count(fout, type, count)
	count -= 1
	t = (count >= 32 ? 1 : 0)
	data = [(t << 7) | ((count & 31) << 2) | type]
	fout.write(data.pack("L"))
	count >>= 5

	while count > 0 do
		t = (count >= 128 ? 1 : 0)
		data = [(t << 7) | (count & 127)]
		fout.write(data.pack("L"))
		count >>= 7
	end
	nil
end

#***************************************************************************/

def resetcounter(total)
	$mycounter_analyze = 0
	$mycounter_encode = 0
	$mycounter_total = total
	nil
end

def setcounter_analyze(n)
	if (n >> 20) != ($mycounter_analyze >> 20) then
		a = (n+64)/128
		e = ($mycounter_encode+64)/128
		d = [($mycounter_total+64)/128, 1].max
		$stderr.write "Analyzing (#{100*a/d}%) Encoding (#{100*e/d}%)\r"
	end
	$mycounter_analyze = n
end

def setcounter_encode(n)
	if (n >> 20) != ($mycounter_encode >> 20) then
		a = ($mycounter_analyze+64)/128
		e = (n+64)/128
		d = [($mycounter_total+64)/128, 1].max
		$stderr.write "Analyzing (#{100*a/d}%) Encoding (#{100*e/d}%)\r"
	end
	$mycounter_encode = n
end

#***************************************************************************/
#*
#* Encode a run of sectors/literals of the same type
#/
def in_flush(edc, type, count, fin, fout)
	write_type_count(fout, type, count)
	if 0 == type then
		while count != 0 do
			b = [2352, count].min
			buf = fin.read(b).unpack("C*")
			edc = edc_computeblock(edc, buf[0..(b-1)])
			fout.write(buf.pack("C*"))
			count -= b
			setcounter_encode(fin.tell)
		end
		return edc
	end

	while count != 0 do
		count -= 1
		case type
		when 1 then
			buf = fin.read(2352).unpack("C*")
			edc = edc_computeblock(edc, buf[0..(2352-1)])
			fout.write(buf[0x00C..(0x00C+0x003-1)].pack("C*"))
			fout.write(buf[0x010..(0x010+0x800-1)].pack("C*"))
			setcounter_encode(fin.tell)
		when 2 then
			buf = fin.read(2336).unpack("C*")
			edc = edc_computeblock(edc, buf[0..(2336-1)])
			fout.write(buf[0x004..(0x004+0x804-1)].pack("C*"))
			setcounter_encode(fin.tell)
		when 3 then
			buf = fin.read(2336).unpack("C*")
			edc = edc_computeblock(edc, buf[0..(2336-1)])
			fout.write(buf[0x004..(0x004+0x918-1)].pack("C*"))
			setcounter_encode(fin.tell)
		end
	end
	return edc
end

#***************************************************************************/

def ecmify(fin, fout)
	inedc = 0
	curtype = -1
	curtypecount = 0
	curtype_in_start = 0
	detecttype = -1
	incheckpos = 0
	inbufferpos = 0
	intotallength = -1
	inqueuestart = 0
	dataavail = 0
	typetally = [0, 0, 0, 0]
	intotallength = File.size(fin)
	resetcounter(intotallength)
	inputqueue = Array.new(1048576 + 4) #unsigned char
	#Magic identifier
	fout.write("ECM\0")
	while true do
		if dataavail < 2352 && dataavail < intotallength - inbufferpos then
			willread = [intotallength - inbufferpos, inputqueue.length - 4 - dataavail].min
			if inqueuestart != 0 then
				inputqueue[4..(4+dataavail-1)] = inputqueue[(4+inqueuestart)..(4+inqueuestart+dataavail-1)]
				inqueuestart = 0
			end
			if willread != 0 then
				setcounter_analyze(inbufferpos)
				fin.seek(inbufferpos, IO::SEEK_SET)
				inputqueue[(4+dataavail)..(4+dataavail+willread-1)] = fin.read(willread).unpack("C*")
				inbufferpos += willread
				dataavail += willread
			end
		end
		break if dataavail <= 0
		detecttype = 0
		if dataavail < 2336 then
			detecttype = 0
		else
			buf1 = inputqueue[[4+inqueuestart-0x10, 0].max..-1]
			buf2 = inputqueue[(4+inqueuestart)..-1]
			detecttype = check_type(buf1, buf2, dataavail >= 2352)
		end
		if detecttype != curtype then
			if curtypecount != 0 then
				fin.seek(curtype_in_start, IO::SEEK_SET)
				typetally[curtype] += curtypecount
				inedc = in_flush(inedc, curtype, curtypecount, fin, fout)
			end
			curtype = detecttype
			curtype_in_start = incheckpos
			curtypecount = 1
		else
			curtypecount += 1
		end
		case curtype
		when 0 then
			incheckpos += 1
			inqueuestart += 1
			dataavail -= 1
		when 1 then
			incheckpos += 2352
			inqueuestart += 2352
			dataavail -= 2352
		when 2 then
			incheckpos += 2336
			inqueuestart += 2336
			dataavail -= 2336
		when 3 then
			incheckpos += 2336
			inqueuestart += 2336
			dataavail -= 2336
		end
	end
	if curtypecount != 0 then
		fin.seek(curtype_in_start, IO::SEEK_SET)
		typetally[curtype] += curtypecount
		inedc = in_flush(inedc, curtype, curtypecount, fin, fout)
	end

	#End-of-records indicator
	write_type_count(fout, 0, 0)
	#Input file EDC
	fout.write([inedc & 0xFF, (inedc >> 8) & 0xFF, (inedc >> 16) & 0xFF, (inedc >> 24) & 0xFF].pack("C*"))
	#Show report
	$stderr.puts "Literal bytes........... #{typetally[0]}"
	$stderr.puts "Mode 1 sectors.......... #{typetally[1]}"
	$stderr.puts "Mode 2 form 1 sectors... #{typetally[2]}"
	$stderr.puts "Mode 2 form 2 sectors... #{typetally[3]}"
	$stderr.puts "Encoded #{intotallength} bytes -> #{fout.tell} bytes"
	$stderr.puts "Done."
	return 0
end

#***************************************************************************/

def main(argc, argv)
	banner()
	#*
	#* Initialize the ECC/EDC tables
	#/
	eccedc_init();
	#*
	#* Check command line
	#/
	if argc != 2 && argc != 3 then
		$stderr.puts "usage: #{argv[0]} cdimagefile [ecmfile]"
		return 1
	end
	infilename = argv[1]
	#*
	#* Figure out what the output filename should be
	#/
	outfilename = nil
	if argc == 3 then
		outfilename = argv[2]
	else
		outfilename = "#{infilename}.ecm"
	end
	$stderr.puts "Encoding #{infilename} to #{outfilename}"

	#*
	#* Open both files
	#/
	File.open(infilename, "r") do |fin|
		File.open(outfilename, "w") do |fout|
			#*
			#* Encode
			#/
			ecmify(fin, fout)
		end
	end
	return 0
end

exit(main(ARGV.length + 1, [File.basename(__FILE__)] + ARGV))
