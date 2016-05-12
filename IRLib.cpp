/* IRLib.cpp from IRLib - an Arduino library for infrared encoding and decoding
 * Version 1.60   January 2016 
 * Copyright 2014-2016 by Chris Young http://cyborg5.com
 * With additions by Gabriel Staples (www.ElectricRCAircraftGuy.com); see CHANGELOG.txt 
 *
 * This library is a major rewrite of IRemote by Ken Shirriff which was covered by
 * GNU LESSER GENERAL PUBLIC LICENSE which as I read it allows me to make modified versions.
 * That same license applies to this modified version. See his original copyright below.
 * The latest Ken Shirriff code can be found at https://github.com/shirriff/Arduino-IRremote
 * My purpose was to reorganize the code to make it easier to add or remove protocols.
 * As a result I have separated the act of receiving a set of raw timing codes from the act of decoding them
 * by making them separate classes. That way the receiving aspect can be more black box and implementers
 * of decoders and senders can just deal with the decoding of protocols. It also allows for alternative
 * types of receivers independent of the decoding. This makes porting to different hardware platforms easier.
 * Also added provisions to make the classes base classes that could be extended with new protocols
 * which would not require recompiling of the original library nor understanding of its detailed contents.
 * Some of the changes were made to reduce code size such as unnecessary use of long versus bool.
 * Some changes were just my weird programming style. Also extended debugging information added.
 */
/*
 * IRremote
 * Version 0.1 July, 2009
 * Copyright 2009 Ken Shirriff
 * For details, see http://www.righto.com/2009/08/multi-protocol-infrared-remote-library.html http://www.righto.com/
 *
 * Interrupt code based on NECIRrcv by Joe Knapp
 * http://www.arduino.cc/cgi-bin/yabb2/YaBB.pl?num=1210243556
 * Also influenced by http://zovirl.com/2008/11/12/building-a-universal-remote-with-an-arduino/
 */

#include "IRLib.h"
#include "IRLibMatch.h"
#include "IRLibRData.h"
#include "IRLibTimer.h"
#include <Arduino.h>
#include <util/atomic.h> //for ATOMIC_BLOCK macro (source: http://www.nongnu.org/avr-libc/user-manual/group__util__atomic.html)

volatile irparams_t irparams; //MUST be volatile since it is used both inside and outside ISRs

/*
 * Returns a pointer to a flash stored string that is the name of the protocol received. 
 */
const __FlashStringHelper *Pnames(IR_types_t Type) {
  if(Type>LAST_PROTOCOL) Type=UNKNOWN;
  // You can add additional strings before the entry for hash code.
  const __FlashStringHelper *Names[LAST_PROTOCOL+1]={F("Unknown"),F("NEC"),F("Sony"),F("RC5"),F("RC6"),F("Panasonic Old"),F("JVC"),F("NECx"),F("Panasonic"),F("Samsung32"),F("Hash Code")};
  return Names[Type];
};


#define TOPBIT 0x80000000

/*
 * The IRsend classes contain a series of methods for sending various protocols.
 * Each of these begin by calling enableIROut(unsigned char kHz) to set the carrier frequency.
 * It then calls mark(int usec) and space(inc usec) to transmit marks and
 * spaces of varying length of microseconds however the protocol defines.
 * Because we want to separate the hardware specific portions of the code from the general programming
 * portions of the code, the code for IRsendBase::IRsendBase, IRsendBase::enableIROut, 
 * IRsendBase::mark and IRsendBase::space can be found in the lower section of this file.
 */

/*
 * Most of the protocols have a header consisting of a mark/space of a particular length followed by 
 * a series of variable length mark/space signals.  Depending on the protocol they very the lengths of the 
 * mark or the space to indicate a data bit of "0" or "1". Most also end with a stop bit of "1".
 * The basic structure of the sending and decoding these protocols led to lots of redundant code. 
 * Therefore I have implemented generic sending and decoding routines. You just need to pass a bunch of customized 
 * parameters and it does the work. This reduces compiled code size with only minor speed degradation. 
 * You may be able to implement additional protocols by simply passing the proper values to these generic routines.
 * The decoding routines do not encode stop bits. So you have to tell this routine whether or not to send one.
 */
void IRsendBase::sendGeneric(unsigned long data, unsigned char Num_Bits, unsigned int Head_Mark, unsigned int Head_Space, 
                             unsigned int Mark_One, unsigned int Mark_Zero, unsigned int Space_One, unsigned int Space_Zero, 
							 unsigned char kHz, bool Use_Stop, unsigned long Max_Extent) {
  Extent=0;
  data = data << (32 - Num_Bits);
  enableIROut(kHz);
//Some protocols do not send a header when sending repeat codes. So we pass a zero value to indicate skipping this.
  if(Head_Mark) mark(Head_Mark); 
  if(Head_Space) space(Head_Space);
  for (int i = 0; i <Num_Bits; i++) {
    if (data & TOPBIT) {
      mark(Mark_One);  space(Space_One);
    } 
    else {
      mark(Mark_Zero);  space(Space_Zero);
    }
    data <<= 1;
  }
  if(Use_Stop) mark(Mark_One);   //stop bit of "1"
  if(Max_Extent) {
#ifdef IRLIB_TRACE
    Serial.print("Max_Extent="); Serial.println(Max_Extent);
	Serial.print("Extent="); Serial.println(Extent);
	Serial.print("Difference="); Serial.println(Max_Extent-Extent);
#endif
	space(Max_Extent-Extent); 
	}
	else space(Space_One);
};

//Protocol info: https://techdocs.altium.com/display/FPGA/NEC+Infrared+Transmission+Protocol
//-the base time is 562.5us 
void IRsendNEC::send(unsigned long data)
{
  if (data==REPEAT) {
    enableIROut(38);
    mark (563* 16); space(563*4); mark(563);space(56*173);
  }
  else {
    sendGeneric(data,32, 563*16, 563*8, 563, 563, 563*3, 563, 38, true);
  }
};

/*
 * Sony is backwards from most protocols. It uses a variable length mark and a fixed length space rather than
 * a fixed mark and a variable space. Our generic send will still work. According to the protocol you must send
 * Sony commands at least three times so we automatically do it here, if desired.
 * GS note: I do NOT want the send command to automatically send 3 times; rather, I want it to send only once
 * unless told otherwise. This is because I am using the Sony protocol to send custom digital data for 
 * wireless control of an RC car, for instance, and it needs to send what I tell it only once in such cases. 
 */
void IRsendSony::send(unsigned long data, int nbits, bool send3times) {
  byte numTimesToSend;
  if (send3times==true)
    numTimesToSend = 3;
  else
    numTimesToSend = 1;
  for(byte i=0; i<numTimesToSend; i++){
     sendGeneric(data,nbits, 600*4, 600, 600*2, 600, 600, 600, 40, false,((nbits==8)? 22000:45000)); 
  }
};

/*
 * This next section of send routines were added by Chris Young. They all use the generic send.
 */
void IRsendNECx::send(unsigned long data)
{
  sendGeneric(data,32, 563*8, 563*8, 563, 563, 563*3, 563, 38, true, 108000);
};

void IRsendPanasonic_Old::send(unsigned long data)
{
  sendGeneric(data,22, 833*4, 833*4, 833, 833, 833*3, 833,57, true);
};

/*
 * JVC omits the mark/space header on repeat sending. Therefore we multiply it by 0 if it's a repeat.
 * The only device I had to test this protocol was an old JVC VCR. It would only work if at least
 * 2 frames are sent separated by 45us of "space". Therefore you should call this routine once with
 * "First=true" and it will send a first frame followed by one repeat frame. If First== false,
 * it will only send a single repeat frame.
 */
void IRsendJVC::send(unsigned long data, bool First)
{
  sendGeneric(data, 16,525*16*First, 525*8*First, 525, 525,525*3, 525, 38, true);
  space(525*45);
  if(First) sendGeneric(data, 16,0,0, 525, 525,525*3, 525, 38, true);
}

/*
 * The remaining protocols require special treatment. They were in the original IRremote library.
 */
void IRsendRaw::send(unsigned int buf[], unsigned char len, unsigned char hz)
{
  enableIROut(hz);
  for (unsigned char i = 0; i < len; i++) {
    if (i & 1) {
      space(buf[i]);
    } 
    else {
      mark(buf[i]);
    }
  }
  space(0); // Just to be sure
}

//Panasonic part is a copy from https://github.com/cyborg5/IRLib/pull/15/commits/a0929cef76391a7035c97368c3ba611ec22aa190
void IRsendPanasonic::PutBits (unsigned long data, int nbits){  
  for (int i = 0; i < nbits; i++) {  
     if (data & 0x80000000) {  
       mark(432);  space(1296);  
      } else {  
       mark(432);  space(432);  
      };  
  data <<= 1;  
  }  
}  

void IRsendPanasonic::send(unsigned long data) {  
  // Enable IR out at 37kHz  
  enableIROut(37);  

  // Send the header  
  mark(3456); space(1728);  

  // Send Panasonic identifier  
  PutBits (2,8);  
  PutBits (32,8);  

  // Send the device, sub-device and function  
  PutBits (data, 24);//Send 12 bits  

  // Send the checksum  
  int checksum = 0;  
  while (data > 0) {  
    checksum = checksum ^ (data & 0xFF);  
    data >>= 8;  
  }  
  PutBits (checksum, 8);  

  // Send the stop bit  
  mark(432);  

  // Lead out is 173 times the base time 432  
  // The U makes sure the compiler doesn't freak out about it being a possible overflow  
  space(172U*432U);  
};  

void IRsendSamsung32::send(unsigned long data)
{
  sendGeneric(data,32, 560*16, 560*8, 560, 560, 560*3, 560, 38, true, 108000);
};
/*
 * The RC5 protocol uses a phase encoding of data bits. A space/mark pair indicates "1"
 * and a mark/space indicates a "0". It begins with a single "1" bit which is not encoded
 * in the data. The high order data bit is a toggle bit that indicates individual
 * keypresses. You must toggle this bit yourself when sending data.
 */

#define RC5_T1		889
#define RC5_RPT_LENGTH	46000
void IRsendRC5::send(unsigned long data)
{
  enableIROut(36);
  data = data << (32 - 13);
  Extent=0;
  mark(RC5_T1); // First start bit
//Note: Original IRremote library incorrectly assumed second bit was always a "1"
//bit patterns from this decoder are not backward compatible with patterns produced
//by original library. Uncomment the following two lines to maintain backward compatibility.
  //space(RC5_T1); // Second start bit
  //mark(RC5_T1); // Second start bit
  for (unsigned char i = 0; i < 13; i++) {
    if (data & TOPBIT) {
      space(RC5_T1); mark(RC5_T1);// 1 is space, then mark
    } 
    else {
      mark(RC5_T1);  space(RC5_T1);// 0 is mark, then space
    }
    data <<= 1;
  }
  space(114000-Extent); // Turn off at end
}

/*
 * The RC6 protocol also phase encodes databits although the phasing is opposite of RC5.
 */
#define RC6_HDR_MARK	2666
#define RC6_HDR_SPACE	889
#define RC6_T1		444
void IRsendRC6::send(unsigned long data, unsigned char nbits)
{
  enableIROut(36);
  data = data << (32 - nbits);
  Extent=0;
  mark(RC6_HDR_MARK); space(RC6_HDR_SPACE);
  mark(RC6_T1);  space(RC6_T1);// start bit "1"
  int t;
  for (int i = 0; i < nbits; i++) {
    if (i == 3) {
      t = 2 * RC6_T1;       // double-wide trailer bit
    } 
    else {
      t = RC6_T1;
    }
    if (data & TOPBIT) {
      mark(t); space(t);//"1" is a Mark/space
    } 
    else {
      space(t); mark(t);//"0" is a space/Mark
    }
    data <<= 1;
  }
  space(107000-Extent); // Turn off at end
}

/*
 * This method can be used to send any of the supported types except for raw and hash code.
 * There is no hash code send possible. You can call sendRaw directly if necessary.
 * Typically "data2" is the number of bits.
 */
void IRsend::send(IR_types_t Type, unsigned long data, unsigned int data2, bool autoRepeatSend) {
  switch(Type) {
    case NEC:           IRsendNEC::send(data); break;
    case SONY:          IRsendSony::send(data,data2,autoRepeatSend); break;
    case RC5:           IRsendRC5::send(data); break;
    case RC6:           IRsendRC6::send(data,data2); break;
    case PANASONIC_OLD: IRsendPanasonic_Old::send(data); break;
    case NECX:          IRsendNECx::send(data); break;    
    case JVC:           IRsendJVC::send(data,(bool)data2); break;
    case PANASONIC      IRsendPanasonic::send(data); break;
    case SAMSUNG32:     IRsendSamsung32::send(data); break;
    
  //case ADDITIONAL:    IRsendADDITIONAL::send(data); break;//add additional protocols here
	//You should comment out protocols you will likely never use and/or add extra protocols here
  }
}

/*
 * The irparams definitions which were located here have been moved to IRLibRData.h
 */

 /*
 * We've chosen to separate the decoding routines from the receiving routines to isolate
 * the technical hardware and interrupt portion of the code which should never need modification
 * from the protocol decoding portion that will likely be extended and modified. It also allows for
 * creation of alternative receiver classes separate from the decoder classes.
 */
IRdecodeBase::IRdecodeBase(void) {
  //atomic guards not needed for these single byte volatile variables 
  irparams.rawbuf2 = this->rawbuf = irparams.rawbuf1;
  
  ignoreHeader=false;
  reset();
};

/*
 * Use External Buffer:
 * NB: The ISR always stores data directly into irparams.rawbuf2, which is *normally* the same buffer
 * as irparams.rawbuf1. However, if you want to use a double-buffer so you can continue to receive
 * new data while decoding the previous IR code then you can define a separate buffer in your 
 * Arduino sketch and and pass the address here.
 * -See IRrecvBase::getResults, and the extensive buffer notes in IRLibRData.h, for more info.
 */
void IRdecodeBase::useDoubleBuffer(volatile uint16_t *p_buffer){
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
  {
    irparams.rawbuf2 = p_buffer; //atomic guards required for volatile pointers since they are multi-byte 
    irparams.doubleBuffered = true;
  }
};

//GS note: 29 Jan 2016: DEPRECATED: copyBuf no longer necessary since the decoder's rawbuf is now the *same buffer* as irparams.rawbuf1. IRhashdecode will be updated to work without copyBuf. 
/*
 * Copies rawbuf and rawlen from one decoder to another. See IRhashdecode example
 * for usage.
 */
/* void IRdecodeBase::copyBuf (IRdecodeBase *source){
  //ensure atomic access in case you are NOT using an external buffer, in which case rawbuf is volatile 
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
  {
    memcpy((void *)rawbuf,(const void *)source->rawbuf,sizeof(irparams.rawbuf1));
  }
  rawlen=source->rawlen;
}; */

/*
 * This routine is actually quite useful. Allows extended classes to call their parent
 * if they fail to decode themselves.
 */
bool IRdecodeBase::decode(void) {
  return false;
};

void IRdecodeBase::reset(void) {
  decode_type= UNKNOWN;
  value=0;
  bits=0;
  rawlen=0;
};
#ifndef USE_DUMP
void DumpUnavailable(void) {Serial.println(F("dumpResults unavailable"));}
#endif
/*
 * This method dumps useful information about the decoded values.
 */
void IRdecodeBase::dumpResults(void) {
#ifdef USE_DUMP
  uint16_t i;
  unsigned long Extent;
  int interval;
  if(decode_type<=LAST_PROTOCOL){
    Serial.print(F("Decoded ")); Serial.print(Pnames(decode_type));
	Serial.print(F("(")); Serial.print(decode_type,DEC);
    Serial.print(F("): Value:")); Serial.print(value, HEX);
  };
  Serial.print(F(" ("));  Serial.print(bits, DEC); Serial.println(F(" bits)"));
  Serial.print(F("Raw samples(")); Serial.print(rawlen, DEC);
  Serial.print(F("): Long Space:")); Serial.println(rawbuf[0], DEC);
  Serial.print(F("  Head: m")); Serial.print(rawbuf[1], DEC);
  Serial.print(F("  s")); Serial.println(rawbuf[2], DEC);
  int LowSpace= 32767; int LowMark=  32767;
  int HiSpace=0; int HiMark=  0;
  Extent=rawbuf[1]+rawbuf[2];
  for (i = 3; i < rawlen; i++) {
    Extent+=(interval= rawbuf[i]);
    if (i % 2) {
      LowMark=min(LowMark, interval);  HiMark=max(HiMark, interval);
      Serial.print(i/2-1,DEC);  Serial.print(F(":m"));
    } 
    else {
       if(interval>0)LowSpace=min(LowSpace, interval);  HiSpace=max (HiSpace, interval);
       Serial.print(F(" s"));
    }
    Serial.print(interval, DEC);
    int j=i-1;
    if ((j % 2)==1)Serial.print(F("\t"));
    if ((j % 4)==1)Serial.print(F("\t "));
    if ((j % 8)==1)Serial.println();
    if ((j % 32)==1)Serial.println();
  }
  Serial.println();
  Serial.print(F("Extent="));  Serial.println(Extent,DEC);
  Serial.print(F("Mark  min:")); Serial.print(LowMark,DEC);Serial.print(F("\t max:")); Serial.println(HiMark,DEC);
  Serial.print(F("Space min:")); Serial.print(LowSpace,DEC);Serial.print(F("\t max:")); Serial.println(HiSpace,DEC);
  Serial.println();
#else
  DumpUnavailable();
#endif
}

/*
 * Again we use a generic routine because most protocols have the same basic structure. However we need to
 * indicate whether or not the protocol varies the length of the mark or the space to indicate a "0" or "1".
 * If "Mark_One" is zero. We assume that the length of the space varies. If "Mark_One" is not zero then
 * we assume that the length of Mark varies and the value passed as "Space_Zero" is ignored.
 * When using variable length Mark, assumes Head_Space==Space_One. If it doesn't, you need a specialized decoder.
 */
bool IRdecodeBase::decodeGeneric(unsigned char Raw_Count, unsigned int Head_Mark, unsigned int Head_Space, 
                                 unsigned int Mark_One, unsigned int Mark_Zero, unsigned int Space_One, unsigned int Space_Zero) {
// If raw samples count or head mark are zero then don't perform these tests.
// Some protocols need to do custom header work.
  unsigned long data = 0;  unsigned char Max; offset=1;
  if (Raw_Count) {if (rawlen != Raw_Count) return RAW_COUNT_ERROR;}
  if(!ignoreHeader) {
    if (Head_Mark) {
	  if (!MATCH(rawbuf[offset],Head_Mark)) return HEADER_MARK_ERROR(Head_Mark);
	}
  }
  offset++;
  if (Head_Space) {if (!MATCH(rawbuf[offset],Head_Space)) return HEADER_SPACE_ERROR(Head_Space);}

  if (Mark_One) {//Length of a mark indicates data "0" or "1". Space_Zero is ignored.
    offset=2;//skip initial gap plus header Mark.
    Max=rawlen;
    while (offset < Max) {
      if (!MATCH(rawbuf[offset], Space_One)) return DATA_SPACE_ERROR(Space_One);
      offset++;
      if (MATCH(rawbuf[offset], Mark_One)) {
        data = (data << 1) | 1;
      } 
      else if (MATCH(rawbuf[offset], Mark_Zero)) {
        data <<= 1;
      } 
      else return DATA_MARK_ERROR(Mark_Zero);
      offset++;
    }
    bits = (offset - 1) / 2;
  }
  else {//Mark_One was 0 therefore length of a space indicates data "0" or "1".
    Max=rawlen-1; //ignore stop bit
    offset=3;//skip initial gap plus two header items
    while (offset < Max) {
      if (!MATCH (rawbuf[offset],Mark_Zero)) return DATA_MARK_ERROR(Mark_Zero);
      offset++;
      if (MATCH(rawbuf[offset],Space_One)) {
        data = (data << 1) | 1;
      } 
      else if (MATCH (rawbuf[offset],Space_Zero)) {
        data <<= 1;
      } 
      else return DATA_SPACE_ERROR(Space_Zero);
      offset++;
    }
    bits = (offset - 1) / 2 -1;//didn't encode stop bit
  }
  // Success
  value = data;
  return true;
}

/*
 * This routine has been modified significantly from the original IRremote.
 * It assumes you've already called IRrecvBase::getResults and it was true.
 * The purpose of getResults is to determine if a complete set of signals
 * has been received. It then copies the raw data into your decoder's rawbuf
 * By moving the test for completion and the copying of the buffer
 * outside of this "decode" method you can use the individual decode
 * methods or make your own custom "decode" without checking for
 * protocols you don't use.
 * Note: Don't forget to call IRrecvBase::resume(); after decoding is complete.
 */
bool IRdecode::decode(void) {
  /*
  GS: Important Note on why I'm NOT USING ATOMIC GUARDS: *technically*, when using a double buffer (see IRLibRData.h for buffer info, to know what a double buffer is) this whole section should be protected with atomic access guards, since we are reading the decoder rawbuf, which points to the volatile irparams.rawbuf1, which is modified periodically by the ISR as follows: whenever a complete new IR code comes in, if double-buffered, the ISR automatically copies its data from irparams.rawbuf2 to rawbuf1, so it can be decoded *while IR receiving continues.* *However,* if you read rawbuf during decoding and it is simultaneously updated by the ISR, you could be reading erroneous or corrupted information. However, this is actually fine in this case, since we are only *reading,* NOT writing. The worst that would happen is the corrupted rawbuf would not be recognized as a valid IR code, or it would be recognized as the wrong code. This can happen during normal receiving anyway, as IR codes are easily distorted during open-air transmission, and sunlight creates a lot of noise. The user's main sketch will simply ignore bad IR codes. Problem solved. 
  -So, WHY NOT PROTECT THIS CODE SEGMENT WITH ATOMIC BLOCK GUARDS? Answer: decoding is waaay too slow! It takes so much time to decode through all of the below code types, that you'd be blocking interrupts for *thousands* or even *tens of thousands* of microseconds, which would totally corrupt any ISR routines and time-stamps anyway! Blocking interrupts for any longer than a few dozen microseconds at most is *bad*! Ex: IRdecodeNEC::decode() alone takes ~2984us. I measured it. If the code being received is one of the lower options, you are looking at it taking up to a couple dozen *milli*seconds. So, just leave the below code alone, and don't protect this particular code. Chances are, in all actuality, that one IR code will be fully decoded long before another IR code arrives anyway, and you will *never* really be at risk of reading rawbuf while rawbuf is being updated by the ISR at the same time. So, I will NOT protect the below code with atomic access guards (ex: via the ATOMIC_BLOCK macro).
  */
  if (IRdecodeNEC::decode()) return true;
  if (IRdecodeSony::decode()) return true;
  if (IRdecodeRC5::decode()) return true;
  if (IRdecodeRC6::decode()) return true;
  if (IRdecodePanasonic_Old::decode()) return true;
  if (IRdecodeNECx::decode()) return true;
  if (IRdecodeJVC::decode()) return true;
  if (IRdecodePanasonic::decode()) return true;
  if (IRdecodeSamsung32::decode()) return true;
  
//if (IRdecodeADDITIONAL::decode()) return true;//add additional protocols here
//Deliberately did not add hash code decoding. If you get decode_type==UNKNOWN and
// you want to know a hash code you can call IRhash::decode() yourself.
// BTW This is another reason we separated IRrecv from IRdecode.
  return false;
}

#define NEC_RPT_SPACE	2250 //562.5*4
//Source for info on protocol timing: https://techdocs.altium.com/display/FPGA/NEC+Infrared+Transmission+Protocol
//562.5us is the base time--the time upon which other times are based 
bool IRdecodeNEC::decode(void) {
  IRLIB_ATTEMPT_MESSAGE(F("NEC"));
  // Check for repeat
  if (rawlen == 4 && MATCH(rawbuf[2], NEC_RPT_SPACE) &&
    MATCH(rawbuf[3],563)) {
    bits = 0;
    value = REPEAT;
    decode_type = NEC;
    return true;
  }
  //                68  ~9000   ~4500  0  563 ~1687.5 563
  if(!decodeGeneric(68, 563*16, 563*8, 0, 563, 563*3, 563)) return false;
  decode_type = NEC;
  return true;
}

// According to http://www.hifi-remote.com/johnsfine/DecodeIR.html#Sony8 
// Sony protocol can only be 8, 12, 15, or 20 bits in length.
bool IRdecodeSony::decode(void) {
  IRLIB_ATTEMPT_MESSAGE(F("Sony"));
  if(rawlen!=2*8+2 && rawlen!=2*12+2 && rawlen!=2*15+2 && rawlen!=2*20+2) return RAW_COUNT_ERROR;
  //                0  2400   600  1200   600  600  0
  if(!decodeGeneric(0, 600*4, 600, 600*2, 600, 600, 0)) return false;
  decode_type = SONY;
  return true;
}

/*
 * The next several decoders were added by Chris Young. They illustrate some of the special cases
 * that can come up when decoding using the generic decoder.
 */

/*
 * A very good source for protocol information is... http://www.hifi-remote.com/johnsfine/DecodeIR.html
 * I used that information to understand what they call the "Panasonic old" protocol which is used by
 * Scientific Atlanta cable boxes. That website uses a very strange notation called IRP notation.
 * For this protocol, the notation was:
 * {57.6k,833}<1,-1|1,-3>(4,-4,D:5,F:6,~D:5,~F:6,1,-???)+ 
 * This indicates that the frequency is 57.6, the base length for the pulse is 833
 * The first part of the <x,-x|x,-x> section tells you what a "0" is and the second part
 * tells you what a "1" is. That means "0" is 833 on, 833 off while an "1" is 833 on
 * followed by 833*3=2499 off. The section in parentheses tells you what data gets sent.
 * The protocol begins with header consisting of 4*833 on and 4*833 off. The other items 
 * describe what the remaining data bits are.
 * It reads as 5 device bits followed by 6 function bits. You then repeat those bits complemented.
 * It concludes with a single "1" bit followed by and an undetermined amount of blank space.
 * This makes the entire protocol 5+6+5+6= 22 bits long since we don't encode the stop bit.
 * The "+" at the end means you only need to send it once and it can repeat as many times as you want.
 */
bool IRdecodePanasonic_Old::decode(void) {
  IRLIB_ATTEMPT_MESSAGE(F("Panasonic_Old"));
  //                48  3332   3332   0  833  2499   833 
  if(!decodeGeneric(48, 833*4, 833*4, 0, 833, 833*3, 833)) return false;
  /*
   * The protocol spec says that the first 11 bits described the device and function.
   * The next 11 bits are the same thing only it is the logical Bitwise complement.
   * Many protocols have such check features in their definition but our code typically doesn't
   * perform these checks. For example NEC's least significant 8 bits are the complement of 
   * of the next more significant 8 bits. While it's probably not necessary to error check this, 
   * you can un-comment the next 4 lines of code to do this extra checking.
   */
//  long S1= (value & 0x0007ff);       // 00 0000 0000 0111 1111 1111 //00000 000000 11111 111111
//  long S2= (value & 0x3ff800)>> 11;  // 11 1111 1111 1000 0000 0000 //11111 111111 00000 000000
//  S2= (~S2) & 0x0007ff;
//  if (S1!=S2) return IRLIB_REJECTION_MESSAGE(F("inverted bit redundancy"));
  // Success
  decode_type = PANASONIC_OLD;
  return true;
}

bool IRdecodeNECx::decode(void) {
  IRLIB_ATTEMPT_MESSAGE(F("NECx"));
  //                68  ~4500  ~4500  0  563 ~1687.5 563
  if(!decodeGeneric(68, 563*8, 563*8, 0, 563, 563*3, 563)) return false;
  decode_type = NECX;
  return true;
}

// JVC does not send any header if there is a repeat.
bool IRdecodeJVC::decode(void) {
  IRLIB_ATTEMPT_MESSAGE(F("JVC"));
  //                36  8400    4200   0  525  1575   525 
  if(!decodeGeneric(36, 525*16, 525*8, 0, 525, 525*3, 525)) 
  {
     IRLIB_ATTEMPT_MESSAGE(F("JVC Repeat"));
     if (rawlen==34) 
     {
        //                0  525  0  0  525  1575   525 
        if(!decodeGeneric(0, 525, 0, 0, 525, 525*3, 525))
           {return IRLIB_REJECTION_MESSAGE(F("JVC repeat failed generic"));}
        else {
 //If this is a repeat code then IRdecodeBase::decode fails to add the most significant bit
           if (MATCH(rawbuf[4],(525*3))) 
           {
              value |= 0x8000;
           } 
           else
           {
             if (!MATCH(rawbuf[4],525)) return DATA_SPACE_ERROR(525);
           }
        }
        bits++;
     }
     else return RAW_COUNT_ERROR;
  } 
  decode_type =JVC;
  return true;
}

bool IRdecodePanasonic::GetBit(void) {  
    if (!MATCH(rawbuf[offset],432)) return DATA_MARK_ERROR(432);  
    offset++;  
    if (MATCH(rawbuf[offset],1296))   
      data = (data << 1) | 1;  
    else if (MATCH(rawbuf[offset],432))   
      data <<= 1;  
    else return DATA_SPACE_ERROR(1296);  
    offset++;  
    return true;  
  };  
    
  bool IRdecodePanasonic::decode(void) {  
    IRLIB_ATTEMPT_MESSAGE(F("Panasonic"));  
    if (rawlen != 100) return RAW_COUNT_ERROR;  
    
    // This handles the lead-in or header  
    if (!MATCH(rawbuf[1],3456))  return HEADER_MARK_ERROR(3456);  
    if (!MATCH(rawbuf[2],1728)) return HEADER_SPACE_ERROR(1728);  
    offset=3;  
     
    // Grab the next two bytes and see if they match 0x4004 which will confirm this is a Panasonic code   
    data = 0;  
    while (offset < 2*8*2+2) if (!GetBit()) return false;  
    // Check if this is 0100000000000100 or 0x4004 in hex  
    if (data != 0x4004) return IRLIB_DATA_ERROR_MESSAGE(F("Error identifying Panasonic"),offset,rawbuf[offset],0x4004);  
    
    // save the next 24 bits to value  
    while(offset < 5*8*2+2) if (!GetBit()) return false;  
    value = data; data = 0;  
    
    decode_type = PANASONIC_NEW;  
    return true;  
  };  

bool IRdecodeSamsung32::decode(void) {
  IRLIB_ATTEMPT_MESSAGE(F("PANASONIC32"));
  //                Estimation based on Lirc.conf file
  if(!decodeGeneric(68, 560*16, 560*8, 0, 560, 560*3, 560)) return false;
  decode_type = PANASONIC32;
  return true;
}
  

/*
 * The remaining protocols from the original IRremote library require special handling
 * This routine gets one undecoded level at a time from the raw buffer.
 * The RC5/6 decoding is easier if the data is broken into time intervals.
 * E.g. if the buffer has MARK for 2 time intervals and SPACE for 1,
 * successive calls to getRClevel will return MARK, MARK, SPACE.
 * offset and used are updated to keep track of the current position.
 * t1 is the time interval for a single bit in microseconds.
 * Returns ERROR if the measured time interval is not a multiple of t1.
 */
IRdecodeRC::RCLevel IRdecodeRC::getRClevel(uint16_t *used, const unsigned int t1) {
  if (offset >= rawlen) {
    // After end of recorded buffer, assume SPACE.
    return SPACE;
  }
  uint16_t width = rawbuf[offset];
  IRdecodeRC::RCLevel val;
  if ((offset) % 2) val=MARK; else val=SPACE;
  
  unsigned char avail;
  if (MATCH(width, t1)) {
    avail = 1;
  } 
  else if (MATCH(width, 2*t1)) {
    avail = 2;
  } 
  else if (MATCH(width, 3*t1)) {
    avail = 3;
  } 
  else {
    if((ignoreHeader) && (offset==1) && (width<t1))
	  avail =1;
	else{
      return ERROR;}
  }
  (*used)++;
  if (*used >= avail) {
    *used = 0;
    (offset)++;
  }
  return val;   
}

#define MIN_RC5_SAMPLES 11
#define MIN_RC6_SAMPLES 1

bool IRdecodeRC5::decode(void) {
  IRLIB_ATTEMPT_MESSAGE(F("RC5"));
  if (rawlen < MIN_RC5_SAMPLES + 2) return RAW_COUNT_ERROR;
  offset = 1; // Skip gap space
  data = 0;
  used = 0;
  // Get start bits
  if (getRClevel(&used, RC5_T1) != MARK) return HEADER_MARK_ERROR(RC5_T1);
//Note: Original IRremote library incorrectly assumed second bit was always a "1"
//bit patterns from this decoder are not backward compatible with patterns produced
//by original library. Uncomment the following two lines to maintain backward compatibility.
  //if (getRClevel(&used, RC5_T1) != SPACE) return HEADER_SPACE_ERROR(RC5_T1);
  //if (getRClevel(&used, RC5_T1) != MARK) return HEADER_MARK_ERROR(RC5_T1);
  for (nbits = 0; offset < rawlen; nbits++) {
    RCLevel levelA = getRClevel(&used, RC5_T1); 
    RCLevel levelB = getRClevel(&used, RC5_T1);
    if (levelA == SPACE && levelB == MARK) {
      // 1 bit
      data = (data << 1) | 1;
    } 
    else if (levelA == MARK && levelB == SPACE) {
      // zero bit
      data <<= 1;
    } 
    else return DATA_MARK_ERROR(RC5_T1);
  }
  // Success
  bits = 13;
  value = data;
  decode_type = RC5;
  return true;
}

bool IRdecodeRC6::decode(void) {
  IRLIB_ATTEMPT_MESSAGE(F("RC6"));
  if (rawlen < MIN_RC6_SAMPLES) return RAW_COUNT_ERROR;
  // Initial mark
  if (!ignoreHeader) {
    if (!MATCH(rawbuf[1], RC6_HDR_MARK)) return HEADER_MARK_ERROR(RC6_HDR_MARK);
  }
  if (!MATCH(rawbuf[2], RC6_HDR_SPACE)) return HEADER_SPACE_ERROR(RC6_HDR_SPACE);
  offset=3;//Skip gap and header
  data = 0;
  used = 0;
  // Get start bit (1)
  if (getRClevel(&used, RC6_T1) != MARK) return DATA_MARK_ERROR(RC6_T1);
  if (getRClevel(&used, RC6_T1) != SPACE) return DATA_SPACE_ERROR(RC6_T1);
  for (nbits = 0; offset < rawlen; nbits++) {
    RCLevel levelA, levelB; // Next two levels
    levelA = getRClevel(&used, RC6_T1); 
    if (nbits == 3) {
      // T bit is double wide; make sure second half matches
      if (levelA != getRClevel(&used, RC6_T1)) return TRAILER_BIT_ERROR(RC6_T1);
    } 
    levelB = getRClevel(&used, RC6_T1);
    if (nbits == 3) {
      // T bit is double wide; make sure second half matches
      if (levelB != getRClevel(&used, RC6_T1)) return TRAILER_BIT_ERROR(RC6_T1);
    } 
    if (levelA == MARK && levelB == SPACE) { // reversed compared to RC5
      // 1 bit
      data = (data << 1) | 1;
    } 
    else if (levelA == SPACE && levelB == MARK) {
      // zero bit
      data <<= 1;
    } 
    else {
      return DATA_MARK_ERROR(RC6_T1); 
    } 
  }
  // Success
  bits = nbits;
  value = data;
  decode_type = RC6;
  return true;
}

/*
 * This Hash decoder is based on IRhashcode
 * Copyright 2010 Ken Shirriff
 * For details see http://www.righto.com/2010/01/using-arbitrary-remotes-with-arduino.html
 * Use FNV hash algorithm: http://isthe.com/chongo/tech/comp/fnv/#FNV-param
 * Converts the raw code values into a 32-bit hash code.
 * Hopefully this code is unique for each button.
 */
#define FNV_PRIME_32 16777619UL
#define FNV_BASIS_32 2166136261UL
// Compare two tick values, returning 0 if newval is shorter,
// 1 if newval is equal, and 2 if newval is longer
int IRdecodeHash::compare(unsigned int oldval, unsigned int newval) {
  if (newval < oldval * .8) return 0;
  if (oldval < newval * .8) return 2;
  return 1;
}

bool IRdecodeHash::decode(void) {
  hash = FNV_BASIS_32;
  for (uint16_t i = 1; i+2 < rawlen; i++) {
    hash = (hash * FNV_PRIME_32) ^ compare(rawbuf[i], rawbuf[i+2]);
  }
//note: does not set decode_type=HASH_CODE nor "value" because you might not want to.
  return true;
}

/* We have created a new receiver base class so that we can use its code to implement
 * additional receiver classes in addition to the original IRremote code which used
 * 50us interrupt sampling of the input pin. See IRrecvLoop and IRrecvPCI classes
 * below. IRrecv is the original receiver class with the 50us sampling.
 */
IRrecvBase::IRrecvBase(unsigned char recvpin)
{
  irparams.recvpin = recvpin; //note: irparams.recvpin is atomically safe since it cannot be modified after object creation 
  init();
}

//initialize IR receiver base object 
void IRrecvBase::init(void) {
  //initialize key global irparams variables
  irparams.LEDblinkActive = false;
  irparams.pauseISR = false;
  irparams.interruptIsDetached = true; 
  //by default, configure for single buffer use (see extensive buffer notes in IRLibRData.h for more info)
  irparams.doubleBuffered = false; 
  
  //initialize IRrecvBase variable:
  Mark_Excess = MARK_EXCESS_DEFAULT;
}

unsigned char IRrecvBase::getPinNum(void){
  return irparams.recvpin;
}

/* Any receiver class must implement a getResults method that will return true when a complete code
 * has been received. At a successful end of your getResults code you should then call IRrecvBase::getResults
 * and it will manipulate the data inside irparams.rawbuf1 (same as decoder.rawbuf), which would have 
 * already been copied into irparams.rawbuf1 from irparams.rawbuf2, if double-buffered, by the ISR. 
 * Some receivers provide results in rawbuf1 measured in ticks on some number of microseconds while others
 * return results in actual microseconds. If you use ticks then you should pass a multiplier
 * value in Time_per_Ticks, in order to convert ticks to us.
 */
bool IRrecvBase::getResults(IRdecodeBase *decoder, const unsigned int Time_per_Tick) {
  decoder->reset();//clear out any old values.
/* Typically IR receivers over-report the length of a mark and under-report the length of a space.
 * This routine adjusts for that by subtracting Mark_Excess from recorded marks and
 * adding it to recorded spaces. The amount of adjustment used to be defined in IRLibMatch.h.
 * It is now user adjustable with the old default of 100
 */
  //ensure atomic access to volatile variables; irparams is volatile
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
  {
    decoder->rawlen = irparams.rawlen1;
  }
  
  //GS Note: I am intentionally choosing *not* to use atomic access guards here below. This is because the effect of not using them is minimal, but using them around the whole "for" loop below can lock out interrupts for up to 200~300+us, which I want to avoid. I could move the atomic access guards to be just on the inside of the "for" loop, so interrupts can run between the "for" loop iterations, but I'm ok with the risk of just not using them at all in this particular instance, as the effect of data corruption here would be minimal anyway. For more information and logic behind this decision, see my note called "Important Note on why I'm NOT USING ATOMIC GUARDS", under the IRdecode::decode(void) function definition above. 
  for(uint16_t i=0; i<decoder->rawlen; i++) 
  {
    //Note: even indices are marks, odd indices are spaces. Subtract Mark_Exces from marks and add it to spaces.
    //-GS UPDATE Note: 29 Jan 2016: decoder->rawbuf now points to the *same buffer* as irparams.rawbuf1, so they are actually interchangeable. 
    decoder->rawbuf[i]=decoder->rawbuf[i]*Time_per_Tick + ( (i % 2)? -Mark_Excess:Mark_Excess);
  }

  return true;
}

void IRrecvBase::enableIRIn(void) { 
  pinMode(irparams.recvpin, INPUT_PULLUP); //many IR receiver datasheets recommend a >10~20K pullup resistor from the output line to 5V; using INPUT_PULLUP does just that
  resume(); //call the child (derived) class's resume function (ex: IRrecvPCI::resume)
}

//Note: one place resume is called is in IRrecvBase::enableIRIn
void IRrecvBase::resume() {
  //ensure atomic access to volatile variables 
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
  {  
    irparams.rawlen1 = irparams.rawlen2 = 0;
    irparams.dataStateChangedToReady = false; //initialize, for use by IRrecv & IRrecvPCI 
  }
}

/* This receiver uses no interrupts or timers. Other interrupt driven receivers
 * allow you to do other things and call getResults at your leisure to see if perhaps
 * a sequence has been received. Typically you would put getResults in your loop
 * and it would return false until the sequence had been received. However because this
 * receiver uses no interrupts, it takes control of your program when you call getResults
 * and doesn't let go until it's got something to show you. The advantage is you don't need
 * interrupts which would make it easier to use and nonstandard hardware and will allow you to
 * use any digital input pin. Timing of this routine is only as accurate as your "micros();"
 * GS Notes: double-buffer doesn't make sense for IRrecvLoop, so we will store data directly
 * into irparams.rawbuf1 directly, whereas an ISR would store it into irparams.rawbuf2 instead.
 */
bool IRrecvLoop::getResults(IRdecodeBase *decoder) {
  bool Finished=false;
  byte OldState=HIGH;byte NewState;
  unsigned long StartTime, DeltaTime, EndTime;
  StartTime=micros();
  while(irparams.rawlen1<RAWBUF) {  //While the buffer not overflowing
    while(OldState==(NewState=digitalRead(irparams.recvpin))) { //While the pin hasn't changed
      if( (DeltaTime = (EndTime=micros()) - StartTime) > 10000) { //If it's a very long wait
        if((Finished=irparams.rawlen1)) break; //finished unless it's the opening gap
      }
    }
    if(Finished) break;
	do_Blink(!NewState);
    irparams.rawbuf1[irparams.rawlen1++]=DeltaTime;
    OldState=NewState;StartTime=EndTime;
  };
  IRrecvBase::getResults(decoder);
  return true;
}
#ifdef USE_ATTACH_INTERRUPTS
/* This receiver uses the pin change hardware interrupt to detect when your input pin
 * changes state. It gives more detailed results than the 50us interrupts of IRrecv
 * and theoretically is more accurate than IRrecvLoop. However because it only detects
 * pin changes, it doesn't always know when it's finished. getResults attempts to detect
 * a long gap of space but sometimes the next signal gets there before getResults notices.
 * This means the second set of signals can get messed up unless there is a long gap.
 * This receiver is based in part on Arduino firmware for use with AnalysIR IR signal analysis
 * software for Windows PCs. Many thanks to the people at http://analysir.com for their 
 * assistance in developing this section of code.
 */

IRrecvPCI::IRrecvPCI(unsigned char inum) {
  init();
  intrnum=inum;
  irparams.recvpin=Pin_from_Intr(inum);
}

//---------------------------------------------------------------------------------------
//checkForEndOfIRCode
//By Gabriel Staples (www.ElectricRCAircraftGuy.com) on 27 Jan 2016
//-a global function for use inside and outside an ISR, by IRrecvPCI
//-this is a non-reentrant function, since it contains static variables & is shared with an ISR, so 
// whenever you call it from outside an ISR, ***put atomic guards around the whole function call, AND
// around the portion of code just before that, where dt is determined.*** See IRrecvPCI::getResults 
// for an example
//--ie: this function will also be called by IRrecPCI::getResults, outside of ISRs, so be sure 
//  to use atomic access guards!
//-pass in the IR receiver input pin pinState, and the time elapsed (dt) in us since the last Mark or Space
// edge occurred. 
//-returns true if dataStateChangedToReady==true, which means dataStateisReady just transitioned 
// from false to true, so the user can now decode the results 
//---------------------------------------------------------------------------------------
//whoIsCalling defines:
#define CALLED_BY_USER (0)
#define CALLED_BY_ISR (1)
bool checkForEndOfIRCode(bool pinState, unsigned long dt, byte whoIsCalling)
{
  //local variables 
  static bool dataStateIsReady_old = true; //the previous data state last time this function was called; initialize to true
  bool dataStateIsReady; 
  bool dataStateChangedToReady = false;

  //Check for long Space to indicate end of IR code 
  //-if the USER is calling this function, we want the pinState to be HIGH (SPACE_START), and dt to be long, to consider this to be the end of the IR code; if pinState transitions from HIGH to LOW, and dt is long, we will let the ISR catch and handle it, rather than the user's call
  //-if the ISR is calling this function, we want the pinState to be LOW (MARK_START), and dt to be long , to consider this to be the end of the IR code, since the ISR is only called when pin state *transitions* occur 
  //-note: "irparams.rawlen2>1" was added to ensure that there actually is data that has been acquired 
  if ((whoIsCalling==CALLED_BY_ISR && pinState==MARK_START && dt>=LONG_SPACE_US && irparams.rawlen2>1) || 
      (whoIsCalling==CALLED_BY_USER && pinState==HIGH && dt>=LONG_SPACE_US && irparams.rawlen2>1)) //a long SPACE gap (10ms or more) just occurred; this indicates the end of a complete IR code 
  {
    dataStateIsReady = true; //the current data state; true since we just detected the end of the IR code 
    
    //check for a *change of state*; only copy the buffer if the change of state just went from false to true
    //-necessary since this function can be repeatedly called by a user via getResults, regardless of whether or not any new data comes in 
    if (dataStateIsReady==true && dataStateIsReady_old==false)
    {
      //data is now ready to be decoded
      dataStateChangedToReady = true; 
      
      if (whoIsCalling==CALLED_BY_ISR)
        irparams.dataStateChangedToReady = true; //used to notify the user that data state just changed to ready, next time the user calls getResults
      else if (whoIsCalling==CALLED_BY_USER)
        irparams.dataStateChangedToReady = false; //this whole function will return true, but since the user is reading this now (calling this whole function from within getResults), and can choose to act on it to decode the data now, it gets immediately reset back to false; otherwise, the user would accidentally try to decode the same data more than once simply by repeatedly calling getResults rapidly. 

      if (irparams.doubleBuffered==true)
      {
        //copy buffer from secondary (rawlen2) to primary (rawlen1); the primary buffer will be waiting for the user to decode it, while the secondary buffer will be written in by this ISR as any new data comes in; see buffer notes in IRLibRData.h for much more info.
        for(unsigned char i=0; i<irparams.rawlen2; i++) 
          irparams.rawbuf1[i] = irparams.rawbuf2[i];
      }
      else //irparams.doubleBuffered==false; for single-buffering:
      {
        irparams.pauseISR = true; //since single-buffered only, we must pause the reception of data until decoding the current data is complete
        //no need to copy anything from irparams.rawbuf2 to irparams.rawbuf1, because when single-buffered, irparams.rawbuf2 points to irparams.rawbuf1 anyway, so they are the same buffer
      }
      irparams.rawlen1 = irparams.rawlen2;
      irparams.rawlen2 = 0; //reset index; start of a new IR code 
    }
  }
  else //end of IR code NOT found yet 
  {
    dataStateIsReady = false; 
  }
  dataStateIsReady_old = dataStateIsReady; //update 
  
  return dataStateChangedToReady;
} //end of checkForEndOfIRCode

//---------------------------------------------------------------------------------------
//Pin Change Interrupt handler/ISR 
//Completely rewritten by Gabriel Staples (www.ElectricRCAircraftGuy.com) on 26 Jan 2016
//-no state machine is used for this ISR anymore 
//-raw codes are optionally double-buffered, in which case raw code values are continually received, and never ignored, 
// even if the user has never called getResults, or has not called it in a long time 
//-the secondary buffer is rawbuf2; a *pointer* to it is stored in irparams, and it is what 
// is used by the ISR to continually store new IR Mark and Space raw values as they come in.
//--the secondary buffer must be *externally created* by the user in their Arduino sketch,
//  and only a *pointer* to it is passed in to irparams; Refer to the 
//  example sketch called "IRrecvPCIDump_UseNoTimers.ino" for an example.
//-the primary buffer is rawbuf1; it is stored in irparams and passed to the decode 
// routines whenever a full sequence is ready to be decoded, and the user calls getResults.
//-The ISR automatically copies the secondary buffer (rawbuf2) to the primary buffer (rawbuf1)
// whenever a full IR code has been received, which is noted by a long space (HIGH pd
// on the IR receiver output pin) of >10ms. 
//---------------------------------------------------------------------------------------
//-Note: HIGH times are either: A) dead-time between codes, or B) code Spaces 
//       LOW times are code Marks
//Defines: now defined in IRLibMatch.h 
//#define MARK_START (LOW) //this edge indicates the start of a mark, and the end of a space, in the IR code sequence
//                         //we are LOW now, so we were HIGH before 
//#define SPACE_START (HIGH) //this edge indicates the start of a space, and the end of a mark, in the IR code sequence
//                           //we are HIGH now, so we were LOW before
void IRrecvPCI_Handler()
{
  if (irparams.pauseISR==true)
    return; //don't process new data if the ISR reception of IR data is paused; pausing is necessary if single-buffered, until old data is decoded, so that it won't be overwritten 
  
  //local vars
  unsigned long t_now = micros(); //us; time stamp this edge
  bool pinState = digitalRead(irparams.recvpin);
  unsigned long t_old = irparams.timer; //us; time stamp last edge (previous time stamp)
  
  //blink LED 
  do_Blink(!pinState);
  
  //check time elapsed 
  unsigned long dt = t_now - t_old; //us; time elapsed ("delta time")
  //Note: if pinState==MARK_START, dt = lastSpaceTime
  //      if pinState==SPACE_START, dt = lastMarkTime
  if (dt < MINIMUM_TIME_GAP_PERMITTED) //consider this last pulse to be noise; ignore it
  {
    return;
  }
  checkForEndOfIRCode(pinState,dt,CALLED_BY_ISR);
  
  //else pinState==MARK_START && (MINIMUM_TIME_GAP_PERMITTED <= dt < LONG_SPACE_US), OR pinState==SPACE_START && (dt >= MINIMUM_TIME_GAP_PERMITTED)
  //process the data by storing the time gap (dt) Mark or Space value 
  irparams.rawbuf2[irparams.rawlen2] = dt; 
  irparams.rawlen2++;
  if (irparams.rawlen2>=RAWBUF)
    irparams.rawlen2 = RAWBUF - 1; //constrain to just keep overwriting the last value, until the start of a new code can be identified again 
  
  irparams.timer = t_now; //us; update 
} //end of IRrecvPCI_Handler()

void IRrecvPCI::enableIRIn(void) {
  IRrecvBase::enableIRIn();
  this->resume();
}

//---------------------------------------------------------------------------------------
//IRrecvPCI::getResults 
//Completely rewritten by Gabriel Staples (www.ElectricRCAircraftGuy.com) on 27 Jan 2016
//-returns true if data is received & ready to be decoded, false otherwise 
//-see the notes just above IRrecvPCI_Handler() & checkForEndOfIRCode for more info. 
//---------------------------------------------------------------------------------------
bool IRrecvPCI::getResults(IRdecodeBase *decoder) 
{
  bool newDataJustIn = false; 
  
  //Order is VERY important here; I'm doing everything in this order for a reason. ~GS:
  //1) first, check to see if irparams.dataStateChangedToReady==true, so we don't needlessly call checkForEndOfIRCode() if data is already ready 
  if (irparams.dataStateChangedToReady==true) //variable is a singe byte; already atomic; atomic guards not needed 
  {
    newDataJustIn = true;
    irparams.dataStateChangedToReady = false; //reset
  }
  else //2) manually check for long Space at end of IR code
  {
    //ensure atomic access to volatile variables
    //-NB: as a MINIMUM, all calcs for dt, *and* the entire checkForEndOfIRCode function call, must be inside the ATOMIC_BLOCK
    //-Note: since digitalRead is slow, I'd like to keep it *outside* the ATOMIC_BLOCK, *if possible*. Here, it *is* possible. Let's consider a case where a pin change interrupt occurs after reading the pinState: I read pinState, an interrupt occurs (pinState changes), I enter the ATOMIC_BLOCK, calculdate dt, and pass in the WRONG pinState but the RIGHT dt to the checkForEndOfIRCode function. What will happen?
    //--Answer: the ISR would have already correctly processed the whole thing, and since checkForEndOfIRCode checks pinState *and* dt, so long as one of those is correct, the same IR code data won't be accidentally processed twice. We should be ok. In this scenario, the dt calcs, *and* the checkForEndOfIRCode, however, *MUST* be inside the *same* ATOMIC_BLOCK for everything to work right. That's why I have done that below.
    bool pinState = digitalRead(irparams.recvpin); //already atomic since irparams.recvpin is one byte 
    // unsigned long t_now = micros(); //us; FOR TESTING 
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
      unsigned long dt = micros() - irparams.timer; //us since last edge; note: irparams.timer contains the last time stamp for a Mark or Space edge 
      newDataJustIn = checkForEndOfIRCode(pinState,dt,CALLED_BY_USER);
    }
    // unsigned long dt = micros() - t_now; //us; FOR TESTING 
    // Serial.print("dt = "); Serial.println(dt); //FOR TESTING; double-buffered result: dt = ~8us normally, or ~144us when a 68-sample NEC code comes in and gets copied over from rawbuf2 to rawbuf1 
  }
  //3) if new data is ready, process it 
  if (newDataJustIn==true)
    IRrecvBase::getResults(decoder); //mandatory to call whenever a new IR data packet is ready to be decoded; this copies volatile data from the secondary buffer into the decoder, while subtracting Mark_Exces from Marks, and adding it to Spaces, among other things
  //4) detach the interrupt if the ISR is paused (the ISR will automatically set the pauseISR flat to true to pause itself whenever a full IR code comes in if it is single-buffered instead of double-buffered)
  if (irparams.pauseISR==true) //note: pauseISR is a single byte and already atomic; no atomic guards needed 
    this->detachInterrupt();
    
  return newDataJustIn;
};

//---------------------------------------------------------------------------------------
//IRrecvPCI::detachInterrupt
//By Gabriel Staples (www.ElectricRCAircraftGuy.com) on 29 Jan 2016
//-disable the ISR so that it will no longer interrupt the code 
//---------------------------------------------------------------------------------------
void IRrecvPCI::detachInterrupt()
{
  ::detachInterrupt(intrnum); //Note: the "::" tells the compiler to use the "global namespace" to find this function--in other words, this is calling the the Arduino core detachInterrupt function, rather than recursively calling the IRrecvPCI::detachInterrupt function. (see here: http://stackoverflow.com/questions/13322530/c-global-structure-creates-name-conflict)
  irparams.interruptIsDetached = true;
}

//---------------------------------------------------------------------------------------
//IRrecvPCI::resume 
//By Gabriel Staples (www.ElectricRCAircraftGuy.com) on 29 Jan 2016
//-resume data collection 
//-this function should be called by the user in only 2 cases:
//--1) when using a single (not double) buffer, you need to resume after you are done calling any decoder functions you need, such as decode or dumpResults
//--2) to manually resume IR receives after the you have manually paused them by calling the receiver's detachInterrupt() function; this applies whether single OR double-buffered 
//---------------------------------------------------------------------------------------
void IRrecvPCI::resume()
{
  //note: atomic guards not needed for single-byte volatile variables 
  if (irparams.interruptIsDetached==true) //Note: interruptIsDetached will *always* be true if either A) we are single-buffered, the ISR set pauseISR to true, and then the user called getResults, or B) the user called the IRrecv::detachInterrupt function directly. We ONLY want to do all this stuff if one of the above events happened. Otherwise, we want to NOT do the following things, such as in the event we are double-buffered, but the user accidentally called resume() anyway, which they should not do.
  {
    irparams.pauseISR = false; //already atomic--no atomic guards required; re-allow ISR data collection
    //set up/re-attach External interrupt 
    irparams.interruptIsDetached = false; //reset 
    attachInterrupt(intrnum, IRrecvPCI_Handler, CHANGE);
    IRrecvBase::resume(); //reset rawlen1 & 2 to 0, among other things 
  }
}

 /* This class facilitates detection of frequency of an IR signal. Requires a TSMP58000
 * or equivalent device connected to the hardware interrupt pin.
 * Create an instance of the object passing the interrupt number.
 */
//These variables MUST be volatile since they are used both in and outside ISRs
volatile unsigned FREQUENCY_BUFFER_TYPE *IRfreqTimes; //non-volatile pointer to volatile data (http://www.barrgroup.com/Embedded-Systems/How-To/C-Volatile-Keyword)
volatile unsigned char IRfreqCount;
IRfrequency::IRfrequency(unsigned char inum) {  //Note this is interrupt number, not pin number
  intrnum=inum;
  pin= Pin_from_Intr(inum);
  //ISR cannot be passed parameters. If I declare the buffer global it would
  //always eat RAN even if this object was not declared. So we make global pointer
  //and copy the address to it. ISR still puts data in the object.
  IRfreqTimes = &(Time_Stamp[0]);
};

// Note ISR handler cannot be part of a class/object
void IRfreqISR(void) {
   IRfreqTimes[IRfreqCount++]=micros();
}

void IRfrequency::enableFreqDetect(void){
  attachInterrupt(intrnum,IRfreqISR, FALLING);
  //ensure atomic access to volatile variables 
  for(i=0; i<256; i++) 
  {
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) //better to have the ATOMIC_BLOCK *inside* the "for" loop so that interrupts can still occur between loop iterations 
    {
      Time_Stamp[i]=0; //Time_Stamp is volatile
    }
  }
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
  {
    IRfreqCount=0; //volatile variable 
  }
  results=0.0;
  samples=0;
};
/* Test to see if we have collected at least one full buffer of data.
 * Note values are always zeroed before beginning so any non-zero data
 * in the final elements means we have collected at least a buffer full.
 * By chance the final might be zero so we test two of them. Would be
 * nearly impossible for two consecutive elements to be zero unless
 * we had not yet collected data.
 */
bool IRfrequency::haveData(void) {
  bool dataIsReceived;
  //ensure atomic access to volatile variables 
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
  {
    dataIsReceived = Time_Stamp[255] || Time_Stamp[254];
  }
  return dataIsReceived;
};

void IRfrequency::disableFreqDetect(void){
  detachInterrupt(intrnum);
 };

//compute the incoming frequency in kHz and store into public variable IRfrequency.results
void IRfrequency::computeFreq(void){
  samples=0; sum=0;
  for(i=1; i<256; i++) {
    unsigned char interval;
    //ensure atomic access to volatile variables; Time_Stamp is volatile here
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
      interval=Time_Stamp[i]-Time_Stamp[i-1];
    }
    if(interval>50 || interval<10) continue;//ignore extraneous results where freq is outside the 20~100kHz range
                                            //Note: 1/50us = 20kHz; 1/10us = 100khz
    sum+=interval;//accumulate usable intervals
    samples++;    //account usable intervals
  };
  if(sum)
    results=(double)samples/(double)sum*1000; //kHz
  else
    results= 0.0;
 };
 
//Didn't need to be a method that we made one following example of IRrecvBase
unsigned char IRfrequency::getPinNum(void) {
  return pin;
}

void IRfrequency::dumpResults(bool Detail) {
  computeFreq();
#ifdef USE_DUMP
  Serial.print(F("Number of samples:")); Serial.print(samples,DEC);
  Serial.print(F("\t  Total interval (us):")); Serial.println(sum,DEC); 
  Serial.print(F("Avg. interval(us):")); Serial.print(1.0*sum/samples,2);
  Serial.print(F("\t Aprx. Frequency(kHz):")); Serial.print(results,2);
  Serial.print(F(" (")); Serial.print(int(results+0.5),DEC);
  Serial.println(F(")"));
  if(Detail) {
    for(i=1; i<256; i++) {
      unsigned int interval;
      //ensure atomic access to volatile variables; Time_Stamp is volatile 
      ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
      {
        interval=Time_Stamp[i]-Time_Stamp[i-1];
      }
      Serial.print(interval,DEC); Serial.print("\t");
      if ((i % 4)==0)Serial.print(F("\t "));
      if ((i % 8)==0)Serial.println();
      if ((i % 32)==0)Serial.println();
    }
    Serial.println();
  }
#else
  DumpUnavailable(); 
#endif
};
#endif // ifdef USE_ATTACH_INTERRUPTS
 
/*
 * The remainder of this file is all related to interrupt handling and hardware issues. It has 
 * nothing to do with IR protocols. You need not understand this is all you're doing is adding 
 * new protocols or improving the receiving, decoding and sending of protocols.
 */

//See IRLib.h comment explaining this function
 unsigned char Pin_from_Intr(unsigned char inum) {
  static const PROGMEM uint8_t attach_to_pin[]= {
#if defined(__AVR_ATmega256RFR2__)//Assume Pinoccio Scout
	4,5,SCL,SDA,RX1,TX1,7
#elif defined(__AVR_ATmega32U4__) //Assume Arduino Leonardo
	3,2,0,1,7
#elif defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)//Assume Arduino Mega 
	2,3, 21, 20, 1, 18
#else	//Assume Arduino Uno or other ATmega328
	2, 3
#endif
  };
#if defined(ARDUINO_SAM_DUE)
  return inum;
#endif
  if (inum<sizeof attach_to_pin) {//note this works because we know it's one byte per entry
	return (byte)pgm_read_byte(&(attach_to_pin[inum]));
  } else {
    return 255;
  }
}

/* 
 * This section contains the hardware specific portions of IRrecvBase
 */
/* If your hardware is set up to do both output and input but your particular sketch
 * doesn't do any output, this method will ensure that your output pin is low
 * and doesn't turn on your IR LED or any output circuit.
 */
void IRrecvBase::noIROutput (void) {
#if defined(IR_SEND_PWM_PIN)
 pinMode(IR_SEND_PWM_PIN, OUTPUT);  
 digitalWrite(IR_SEND_PWM_PIN, LOW); // When not sending PWM, we want it low    
#endif
}

//enable/disable blinking of any arbitrary LED pin whenever IR data comes in 
void IRrecvBase::setBlinkLED(uint8_t pinNum, bool blinkActive)
{
  //atomic access guards not required since these LED parameters are all single bytes 
  //These masks, ports, etc, will be used for auto-mapped direct port access to blink the LED 
  //-this is *much* faster than digitalWrite 
  irparams.LEDpinNum = pinNum;
  irparams.LEDbitMask = digitalPinToBitMask(pinNum);
  irparams.LEDp_PORT_out = portOutputRegister(digitalPinToPort(pinNum));
  irparams.LEDblinkActive = blinkActive;
  if (blinkActive)
     pinMode(irparams.LEDpinNum,OUTPUT);
  else //LEDblinkActive==false 
  {
    pinMode(irparams.LEDpinNum,INPUT);
    fastDigitalWrite(irparams.LEDp_PORT_out, irparams.LEDbitMask, LOW); //digitalWrite to LOW to ensure INPUT_PULLUP is NOT on
  }
}

//kept for backwards compatibility
//-same as setBlinkLED, except the LED is forced to be LED_BUILTIN, which is usually LED 13 
//-see here for info on LED_BUILTIN: https://www.arduino.cc/en/Reference/Constants
void IRrecvBase::blink13(bool blinkActive)
{
  this->setBlinkLED(LED_BUILTIN, blinkActive);
}

//Do the actual blinking off and on
//This is not part of IRrecvBase because it may need to be inside an ISR
//and we cannot pass parameters to them.
void do_Blink(bool blinkState) {
  //atomic access guards not required since these LED parameters are all single bytes and hence, already atomic; also, if this method is called within an ISR, of course it is atomic, as interrupts are by default disabled inside ISRs.
  if (irparams.LEDblinkActive)
    fastDigitalWrite(irparams.LEDp_PORT_out, irparams.LEDbitMask, blinkState);
}

/* If not using the IRrecv class but only using IRrecvPCI or IRrecvLoop you can eliminate
 * some timer conflicts with the duplicate definition of ISR by turning off USE_IRRECV.
 * To do this, simply go to IRLib.h and comment out "#define USE_IRRECV". 
 */
//===========================================================================================
#ifdef USE_IRRECV
//===========================================================================================

/*
 * The original IRrecv which uses 50us timer driven interrupts to sample input pin.
 */

//---------------------------------------------------------------------------------------
//IRrecv::resume 
//By Gabriel Staples (www.ElectricRCAircraftGuy.com) on 30 Jan 2016
//-resume data collection 
//-this function should be called by the user in only 2 cases:
//--1) when using a single (not double) buffer, you need to resume after you are done calling any decoder functions you need, such as decode or dumpResults
//--2) to manually resume IR receives after the you have manually paused them by calling the receiver's detachInterrupt() function; this applies whether single OR double-buffered 
//---------------------------------------------------------------------------------------
void IRrecv::resume() {  
  //note: atomic guards not needed for single-byte volatile variables 
  if (irparams.interruptIsDetached==true) //Note: interruptIsDetached will *always* be true if either A) we are single-buffered, the ISR set pauseISR to true, and then the user called getResults, or B) the user called the IRrecv::detachInterrupt function directly. We ONLY want to do all this stuff if one of the above events happened. Otherwise, we want to NOT do the following things, such as in the event we are double-buffered, but the user accidentally called resume() anyway, which they should not do.
  {
    irparams.pauseISR = false; //already atomic--no atomic guards required; re-allow ISR data collection
    //set up/re-attach External interrupt 
    irparams.interruptIsDetached = false; //reset 
    //initialize state machine variables
    irparams.rcvstate = STATE_START; //atomic since interrupt isn't enabled yet until the next line 
    IR_RECV_ENABLE_INTR; //enable interrupt
    IRrecvBase::resume(); //reset rawlen1 & 2 to 0, among other things 
  }
}

void IRrecv::enableIRIn(void) {
  IRrecvBase::enableIRIn();
  IR_RECV_CONFIG_TICKS(); //set up pulse clock timer interrupt (ex: for every 50us)
  this->resume(); //interrupt is actually enabled here 
}

//Rewritten by Gabriel Staples (www.ElectricRCAircraftGuy.com) on 30 Jan 2016 
bool IRrecv::getResults(IRdecodeBase *decoder) 
{
  bool newDataJustIn = false;
  
  //1) see if new IR data is ready to be processed 
  if (irparams.dataStateChangedToReady==true) //variable is a singe byte; already atomic; atomic guards not needed 
  {
    newDataJustIn = true;
    irparams.dataStateChangedToReady = false; //reset
    //2) 2nd, process the new data  
    IRrecvBase::getResults(decoder,USEC_PER_TICK); //mandatory to call whenever a new IR data packet is ready to be decoded; this copies volatile data from the secondary buffer into the decoder, while subtracting Mark_Exces from Marks, and adding it to Spaces, among other things
  }
  //3) detach the interrupt if the ISR is paused (the ISR will automatically set the pauseISR flag to true to pause itself whenever a full IR code comes in if it is single-buffered instead of double-buffered)
  if (irparams.pauseISR==true) //note: pauseISR is a single byte and already atomic; no atomic guards needed 
    this->detachInterrupt();
    
  return newDataJustIn;
}

//---------------------------------------------------------------------------------------
//IRrecv::detachInterrupt
//By Gabriel Staples (www.ElectricRCAircraftGuy.com) on 29 Jan 2016
//-disable the ISR so that it will no longer interrupt the code 
//---------------------------------------------------------------------------------------
void IRrecv::detachInterrupt()
{
  IR_RECV_DISABLE_INTR;
  irparams.interruptIsDetached = true;
}

//---------------------------------------------------------------------------------------
//ISR for IRrecv 
//Rewritten by Gabriel Staples (www.ElectricRCAircraftGuy.com) on 30 Jan 2016 
//-reads in incoming IR data via this software Interrupt Service Routine which is called every
// 50us. 
/*
This interrupt service routine is only used by IRrecv and may or may not be used by other
extensions of the IRrecvBase. It is timer driven interrupt code to collect raw data.
Widths of alternating SPACE, MARK are recorded in rawbuf2, recorded in ticks of 50 microseconds.
rawlen2 counts the number of entries recorded so far. First entry is the long gap SPACE 
(IR receiver HIGH time) between transmissions. The state machine starts in STATE_START,
then proceeds to STATE_TIMING_MARK to time an incoming MARK, followed by 
STATE_TIMING_SPACE to time an incoming SPACE. These alternate until the long gap SPACE 
between IR codes is found, at which point irparams.dataStateChangedToReadyready is set to
true, while the state machine returns back to STATE_START, and timing of this long 
SPACE continues until STATE_START encounters the next MARK.
Note that once irparams.dataStateChangedToReady is set to true, if single-buffered, 
(see IRLibRData.h for more info on buffers), irparams.pauseISR is set to true until 
the data is decoded, but if double-buffered, IR data receiving, processing, and storing 
resume immediately.
*/
//---------------------------------------------------------------------------------------
ISR(IR_RECV_INTR_NAME)
{
  irparams.timer++; // One more 50us tick
  
  if (irparams.pauseISR==true) //if single-buffered only 
    return; //keep incrementing the timer (hence why it is above this), but don't analyse or store any new incoming IR data until the buffer is no longer in use, and the decoder is done using the buffer data is complete
  
  enum irdata_t {IR_MARK=LOW, IR_SPACE=HIGH}; //IR_MARK is LOW; IR_SPACE is HIGH 
  //read IR receiver incoming pin state (HIGH is a SPACE, LOW is a MARK, since IR receiver is active LOW)
  irdata_t irdata = (irdata_t)digitalRead(irparams.recvpin); 
  
  //Check for buffer overflow 
  if (irparams.rawlen2 >= RAWBUF) { //Buffer overflow
    irparams.rawlen2--; //decrement the rawlen2 value so you just keep overwriting new data onto this final location in the raw buffer array
  }
  
  //State Machine:
  switch(irparams.rcvstate) {
  case STATE_START: //Timing the gap (long SPACE) between IR codes, waiting for first MARK to start 
    //we are waiting for the first MARK to occur, as the start of a new transmission, so ignore SPACES
    if (irdata == IR_MARK) {
      //gap (long SPACE between IR transmissions) just ended, so record long SPACE duration we just measured, and prepare to start recording the first MARK of the transmission 
      irparams.rawlen2 = 0;
      irparams.rawbuf2[irparams.rawlen2++] = irparams.timer;
      irparams.timer = 0;
      irparams.rcvstate = STATE_TIMING_MARK;
    }
    break;
  case STATE_TIMING_MARK: //timing MARK, waiting for next SPACE to start 
    if (irdata==IR_SPACE && irparams.timer>=US_TO_TICKS(MINIMUM_TIME_GAP_PERMITTED)) { //MARK ended, record time; filter out really short MARKS by ensuring the MARK is long enough to not just be noise 
      irparams.rawbuf2[irparams.rawlen2++] = irparams.timer;
      irparams.timer = 0;
      irparams.rcvstate = STATE_TIMING_SPACE;
    }
    break;
  case STATE_TIMING_SPACE: //timing SPACE, waiting for next MARK to start, OR for enough time to elapse that we know the entire IR code is complete (marked by a long SPACE)
    if (irdata==IR_MARK && irparams.timer>=US_TO_TICKS(MINIMUM_TIME_GAP_PERMITTED)) { //SPACE just ended, record its time; filter out really short SPACES by ensuring the SPACE is long enough to not just be noise 
      irparams.rawbuf2[irparams.rawlen2++] = irparams.timer;
      irparams.timer = 0;
      irparams.rcvstate = STATE_TIMING_MARK;
    }
    else if (irdata==IR_SPACE && irparams.timer>US_TO_TICKS(LONG_SPACE_US)) {
      //Big SPACE, indicates gap between codes, which means an IR code just ended!
      //data is now ready to be decoded
      irparams.dataStateChangedToReady = true;
      irparams.rcvstate = STATE_START; //prepare for next code 
      
      //Next: don't reset timer--keep counting space width, then do: A) If single-buffered, set irparams.pauseISR to true, OR B) if double-buffered, copy buffer data over
      if (irparams.doubleBuffered==true)
      {
        //copy buffer from secondary (rawlen2) to primary (rawlen1); the primary buffer will be waiting for the user to decode it, while the secondary buffer will be written in by this ISR as any new data comes in; see buffer notes in IRLibRData.h for much more info.
        for(unsigned char i=0; i<irparams.rawlen2; i++) 
          irparams.rawbuf1[i] = irparams.rawbuf2[i];
      }
      else //irparams.doubleBuffered==false; for single-buffering:
      {
        irparams.pauseISR = true; //since single-buffered only, we must pause the reception of data until decoding the current data is complete
        //no need to copy anything from irparams.rawbuf2 to irparams.rawbuf1, because when single-buffered, irparams.rawbuf2 points to irparams.rawbuf1 anyway, so they are the same buffer
      }
      irparams.rawlen1 = irparams.rawlen2;
    }
    break;
  } //end of switch
  
  do_Blink(!(bool)irdata); //blink LED indicator LED during receiving of IR data 
}
#endif //end of ifdef USE_IRRECV

/*
 * The hardware specific portions of IRsendBase
 */
void IRsendBase::enableIROut(unsigned char khz) {
//NOTE: the comments on this routine accompanied the original early version of IRremote library
//which only used TIMER2. The parameters defined in IRLibTimer.h may or may not work this way.
  // Enables IR output.  The khz value controls the modulation frequency in kilohertz.
  // The IR output will be on pin 3 (OC2B).
  // This routine is designed for 36-40KHz; if you use it for other values, it's up to you
  // to make sure it gives reasonable results.  (Watch out for overflow / underflow / rounding.)
  // TIMER2 is used in phase-correct PWM mode, with OCR2A controlling the frequency and OCR2B
  // controlling the duty cycle.
  // There is no prescaling, so the output frequency is 16MHz / (2 * OCR2A)
  // To turn the output on and off, we leave the PWM running, but connect and disconnect the output pin.
  // A few hours staring at the ATmega documentation and this will all make sense.
  // See my Secrets of Arduino PWM at http://www.righto.com/2009/07/secrets-of-arduino-pwm.html for details.
  
  // Disable the Timer2 Interrupt (which is used for receiving IR)
 IR_RECV_DISABLE_INTR; //Timer2 Overflow Interrupt    
 pinMode(IR_SEND_PWM_PIN, OUTPUT);  
 digitalWrite(IR_SEND_PWM_PIN, LOW); // When not sending PWM, we want it low    
 IR_SEND_CONFIG_KHZ(khz);
 }

IRsendBase::IRsendBase () {
 pinMode(IR_SEND_PWM_PIN, OUTPUT);  
 digitalWrite(IR_SEND_PWM_PIN, LOW); // When not sending PWM, we want it low    
}

//The Arduino built in function delayMicroseconds has limits we wish to exceed
//Therefore we have created this alternative
void  My_delay_uSecs(unsigned int T) {
  if(T){if(T>16000) {delayMicroseconds(T % 1000); delay(T/1000); } else delayMicroseconds(T);};
}

void IRsendBase::mark(unsigned int time) {
 IR_SEND_PWM_START;
 IR_SEND_MARK_TIME(time);
 Extent+=time;
}

void IRsendBase::space(unsigned int time) {
 IR_SEND_PWM_STOP;
 My_delay_uSecs(time);
 Extent+=time;
}

/*
 * Various debugging routines
 */


#ifdef IRLIB_TRACE
void IRLIB_ATTEMPT_MESSAGE(const __FlashStringHelper * s) {Serial.print(F("Attempting ")); Serial.print(s); Serial.println(F(" decode:"));};
void IRLIB_TRACE_MESSAGE(const __FlashStringHelper * s) {Serial.print(F("Executing ")); Serial.println(s);};
byte IRLIB_REJECTION_MESSAGE(const __FlashStringHelper * s) { Serial.print(F(" Protocol failed because ")); Serial.print(s); Serial.println(F(" wrong.")); return false;};
byte IRLIB_DATA_ERROR_MESSAGE(const __FlashStringHelper * s, unsigned char index, unsigned int value, unsigned int expected) {  
 IRLIB_REJECTION_MESSAGE(s); Serial.print(F("Error occurred with rawbuf[")); Serial.print(index,DEC); Serial.print(F("]=")); Serial.print(value,DEC);
 Serial.print(F(" expected:")); Serial.println(expected,DEC); return false;
};
#endif
