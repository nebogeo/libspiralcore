/*
 *  Copyright (C) 2000 David Griffiths <dave@pawfal.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/ 

#include "Midi.h"
#include "unistd.h"
#include "sys/types.h"
#include "signal.h"
#include "pthread.h"

using namespace spiralcore;

static const int MIDI_SCANBUFSIZE=256;
static const int MIDI_KEYOFFSET=0;

static const unsigned char STATUS_START            = 0x80;		
static const unsigned char STATUS_NOTE_OFF         = 0x80;
static const unsigned char STATUS_NOTE_ON          = 0x90;
static const unsigned char STATUS_AFTERTOUCH       = 0xa0;
static const unsigned char STATUS_CONTROL_CHANGE   = 0xb0;
static const unsigned char STATUS_PROG_CHANGE      = 0xc0;
static const unsigned char STATUS_CHANNEL_PRESSURE = 0xd0;
static const unsigned char STATUS_PITCH_WHEEL      = 0xe0;
static const unsigned char STATUS_END              = 0xf0;
static const unsigned char SYSEX_START             = 0xf0;
static const unsigned char SYSEX_TERMINATOR        = 0xf7;
static const unsigned char MIDI_CLOCK              = 0xf8;
static const unsigned char ACTIVE_SENSE            = 0xfe;

static int NKEYS = 30;
							
MidiDevice *MidiDevice::m_Singleton;
string MidiDevice::m_DeviceName;
string MidiDevice::m_AppName; 

#if __APPLE__
#define read	AppleRead
#endif

void MidiDevice::Init(const string &name, Type t) 
{ 
	if (!m_Singleton) 
	{
		m_AppName=name;
		m_Singleton=new MidiDevice(t); 
	}
}
	
MidiDevice::MidiDevice(Type t) :
m_Poly(1),
m_Clock(1.0f),
m_ClockCount(0),
m_Type(t)
{
	Open(t);
}

MidiDevice::~MidiDevice() 
{
#ifdef ALSA_MIDI
	snd_seq_close (seq_handle);
#else
	Close();
#endif
}

void MidiDevice::Close()
{
#if __APPLE__
	AppleClose();
#else
	pthread_mutex_lock(m_Mutex);
	pthread_cancel(m_MidiReader); 
	pthread_mutex_unlock(m_Mutex);
	pthread_mutex_destroy(m_Mutex);
		
	close(m_MidiFd);
	close(m_MidiWrFd);
	cerr<<"Closed midi device"<<endl;
#endif // !__APPLE__
}


void MidiDevice::Open(Type t)
{
#if __APPLE__
	AppleOpen();
#else
#ifdef ALSA_MIDI
if (t==READ) cerr<<"Opening alsa midi for reading"<<endl;
else cerr<<"Opening alsa midi for writing"<<endl;

	seq_handle=AlsaOpen(t);
#else
	//if (!SpiralInfo::WANTMIDI) return;
	
	m_MidiFd = open(m_DeviceName.c_str(),O_RDONLY|O_SYNC);  
	if (!m_MidiFd) 
	{
		cerr<<"Couldn't open midi for reading ["<<m_DeviceName<<"]"<<endl;
		return;
	}
	
	m_MidiWrFd = open(m_DeviceName.c_str(),O_WRONLY);  
	if (!m_MidiWrFd) 
	{
		cerr<<"Couldn't open midi for writing ["<<m_DeviceName<<"]"<<endl;
		return;
	}
	
	cerr<<"Opened midi device ["<<m_DeviceName<<"]"<<endl;
#endif
#endif // !__APPLE__
	
	m_Mutex = new pthread_mutex_t;
    pthread_mutex_init(m_Mutex, NULL);
#ifdef ALSA_MIDI
    if (m_Type==READ) pthread_create(&m_MidiReader,NULL,(void*(*)(void*))MidiDevice::AlsaMidiReaderCallback,(void*)this);	
#else
    pthread_create(&m_MidiReader,NULL,(void*(*)(void*))MidiDevice::MidiReaderCallback,(void*)this);	
#endif
}


// returns the next event off the list, or an 
// empty event if the list is exhausted
MidiEvent MidiDevice::GetEvent(int Device)
{
	if (Device<0 || Device>15)
	{
		cerr<<"GetEvent: Invalid Midi device "<<Device<<endl;
		return MidiEvent(MidiEvent::NONE,0,0);
	}

	pthread_mutex_lock(m_Mutex);
	if (m_EventVec[Device].size()==0)
	{
		pthread_mutex_unlock(m_Mutex);
		return MidiEvent(MidiEvent::NONE,0,0);
	}
	
	MidiEvent event(m_EventVec[Device].front());
	m_EventVec[Device].pop();
	pthread_mutex_unlock(m_Mutex);

	return event;
}

void MidiDevice::SendEvent(int Device,const MidiEvent &Event)
{
#ifdef ALSA_MIDI
	snd_seq_event_t ev;
	snd_seq_ev_clear      (&ev);
  	snd_seq_ev_set_direct (&ev);
	snd_seq_ev_set_subs   (&ev);
    snd_seq_ev_set_source (&ev, 0);
  	
	switch (Event.GetType())
	{
		case MidiEvent::ON : ev.type = SND_SEQ_EVENT_NOTEON; break;
		case MidiEvent::OFF : ev.type = SND_SEQ_EVENT_NOTEOFF; break;
		case MidiEvent::AFTERTOUCH : break;
		case MidiEvent::PARAMETER : break;
		case MidiEvent::CHANNELPRESSURE : break;
		case MidiEvent::PITCHBEND : break;
		case MidiEvent::NONE : break;
	}
	
	ev.data.note.velocity = (char)(Event.GetVolume()*127.0f);
	ev.data.control.channel = Device;
	ev.data.note.note=Event.GetNote();
	
	int ret=snd_seq_event_output(seq_handle, &ev);
	snd_seq_drain_output(seq_handle);
#else
	if (Device<0 || Device>15)
	{
		cerr<<"SendEvent: Invalid Midi device "<<Device<<endl;		
	}

	char message[3];
	
	message[1]=Event.GetNote()+MIDI_KEYOFFSET;
	message[2]=(char)Event.GetVolume();
	
	if (Event.GetType()==MidiEvent::ON)
	{
		message[0]=STATUS_NOTE_ON+Device;
		write(m_MidiWrFd,message,3);
		//cerr<<"sending "<<message<<endl;
	}
	
	if (Event.GetType()==MidiEvent::OFF)
	{
		message[0]=STATUS_NOTE_OFF+Device;
		write(m_MidiWrFd,message,3);	
		//cerr<<"sending "<<message<<endl;
	}
#endif
}

/////////////////////////////////////////////////////////////////////////////////
// Reader thread functions

// little helper to strip out the realtime and unused messages
void MidiDevice::ReadByte(unsigned char *c)
{
	*c=ACTIVE_SENSE;
	do read(m_MidiFd,c,1);				
	while (*c>=STATUS_END && *c!=MIDI_CLOCK);		
}

// collect events deals with the byte level messages, and sorts 
// and filters them into distinct messages we can handle easily
void MidiDevice::CollectEvents()
{		
	unsigned char buf[1];	
	int count,n,nn;
	bool MessageSent;
	unsigned char data[3],last=0;
	
	// constantly scan for relevent input,
	// and write it to the pipe

	// filters out unhandled messages, and attempts to build 
	// coherent messages to send to the midi handler
	bool InSysex=false;
		
	for(;;)
	{
		ReadByte(buf);
		
		if (*buf==MIDI_CLOCK) 
		{
			m_ClockCount++;
			if (m_ClockCount==6)
			{
				m_Clock=-m_Clock;
				m_ClockCount=0;
			}
		}
		else
		if (*buf>=STATUS_START) // we've got a status byte
		{											
			if (*buf==SYSEX_TERMINATOR) InSysex=false;

			// find out if it's an opcode
			if(*buf>=STATUS_START && *buf<=STATUS_END)   
			{				
				InSysex=false;
				last=data[0]=*buf;
			
				if (data[0]>=STATUS_PROG_CHANGE && data[0]<STATUS_PITCH_WHEEL) 
				{
					ReadByte(&data[1]); //one byte
					data[2]=0;
				}
				else // get the next two bytes
				{					
					ReadByte(&data[1]); 
					ReadByte(&data[2]); 
				}
				AddEvent(data);													
			}			
			else // its a sysex or other message like active sense
			{ 
				if (*buf==SYSEX_START) InSysex=true;
				cerr<<"Unhandled midi message: "; printf("%x\n",(int)*buf);				
			}
		}
		else // more data (running status) 
		{
			if (!InSysex) 
			{
				data[0]=last;
				data[1]=*buf;
				
				if (data[0]>=STATUS_PROG_CHANGE && data[0]<STATUS_PITCH_WHEEL) 
				{					
					data[2]=0;  //one byte
				}
				else // get the next byte
				{						
					ReadByte(&data[2]); 					
				}
				
				AddEvent(data);							  											
			}
		}			
	}
}
		
// addevent converts the midi bytecode into midi message objects and 
// stacks them onto the event list to be picked up by the app
void MidiDevice::AddEvent(unsigned char* midi)
{	
	MidiEvent::type MessageType=MidiEvent::NONE;
	int Volume=0,Note=0,EventDevice=0;	
	
	// note off
	if (midi[0] >= STATUS_NOTE_OFF && midi[0] < STATUS_NOTE_ON) 
	{	
		MessageType=MidiEvent::OFF;
		Note=midi[1]-MIDI_KEYOFFSET;
		EventDevice=midi[0]-STATUS_NOTE_OFF;
	}
	// note on
	else if (midi[0] >= STATUS_NOTE_ON && midi[0] < STATUS_AFTERTOUCH)
	{
		Volume = midi[2];
		
		// cope with Roland equipment, where note on's 
		// with zero velocity are sent as note offs.
		if (Volume) MessageType=MidiEvent::ON;
		else MessageType=MidiEvent::OFF;

		Note=midi[1]-MIDI_KEYOFFSET;
		EventDevice=midi[0]-STATUS_NOTE_ON;
	}
	// aftertouch
	else if (midi[0] >= STATUS_AFTERTOUCH && midi[0] < STATUS_CONTROL_CHANGE)
	{			
		MessageType=MidiEvent::AFTERTOUCH;
		Note=midi[1]-MIDI_KEYOFFSET;		
		Volume=midi[2];			
		EventDevice=midi[0]-STATUS_AFTERTOUCH;
	}
	// parameter
	else if (midi[0] >= STATUS_CONTROL_CHANGE && midi[0] < STATUS_PROG_CHANGE)
	{	
		MessageType=MidiEvent::PARAMETER;
		Note=midi[1];
		Volume=midi[2];
		EventDevice=midi[0]-STATUS_CONTROL_CHANGE;
	}
	// channel pressure
	else if (midi[0] >= STATUS_CHANNEL_PRESSURE && midi[0] < STATUS_PITCH_WHEEL)
	{			
		MessageType=MidiEvent::CHANNELPRESSURE;		
		Volume=midi[1];			
		EventDevice=midi[0]-STATUS_CHANNEL_PRESSURE;
	}
	// note pitchbend
	else if (midi[0] >= STATUS_PITCH_WHEEL && midi[0] < STATUS_END)
	{			
		MessageType=MidiEvent::PITCHBEND;
		// should probably take the first byte into account too?
		Volume=midi[2];			
		EventDevice=midi[0]-STATUS_PITCH_WHEEL;
	}
	
	if (EventDevice<0 || EventDevice>15)
	{
		cerr<<"Error - Midi device "<<EventDevice<<" ??"<<endl;
		return;
	}

	pthread_mutex_lock(m_Mutex);
	m_EventVec[EventDevice].push(MidiEvent(MessageType,Note,Volume));
	pthread_mutex_unlock(m_Mutex);
}

///////////////////////////////////////////////////////////////////////////////

#ifdef ALSA_MIDI
// code taken and modified from jack_miniFMsynth

void MidiDevice::AlsaCollectEvents() 
{
    int seq_nfds, l1;
    struct pollfd *pfds;

	seq_nfds = snd_seq_poll_descriptors_count(seq_handle, POLLIN);
	pfds = (struct pollfd *)alloca(sizeof(struct pollfd) * seq_nfds);
    snd_seq_poll_descriptors(seq_handle, pfds, seq_nfds, POLLIN);
   
	for(;;)
	{   
		if (poll (pfds, seq_nfds, 1000) > 0) 
		{
    		for (l1 = 0; l1 < seq_nfds; l1++) 
			{
    	    	if (pfds[l1].revents > 0)
				{				
					snd_seq_event_t *ev;
    				int l1;
					MidiEvent::type MessageType=MidiEvent::NONE;
					int Volume=0,Note=0,EventDevice=0;	
    				do 
					{
        				snd_seq_event_input(seq_handle, &ev);

        				if ((ev->type == SND_SEQ_EVENT_NOTEON) && (ev->data.note.velocity == 0)) 
        				{
							ev->type = SND_SEQ_EVENT_NOTEOFF;      
						}

        				switch (ev->type) {
            				case SND_SEQ_EVENT_PITCHBEND:
                				MessageType=MidiEvent::CHANNELPRESSURE;		
								Volume = (char)((ev->data.control.value / 8192.0)*256);
                				break;
            				case SND_SEQ_EVENT_CONTROLLER:
								MessageType=MidiEvent::PARAMETER;
								Note = ev->data.control.param;
								Volume = ev->data.control.value;
                				break;
            				case SND_SEQ_EVENT_NOTEON:
								MessageType=MidiEvent::ON;
                				EventDevice = ev->data.control.channel;
                				Note = ev->data.note.note;
                				Volume = ev->data.note.velocity;
                				break;        
            				case SND_SEQ_EVENT_NOTEOFF:
								MessageType=MidiEvent::ON;
                				EventDevice = ev->data.control.channel;
                				Note = ev->data.note.note;
                				break;        
        				}
						pthread_mutex_lock(m_Mutex);
						m_EventVec[EventDevice].push(MidiEvent(MessageType,Note,Volume));
						pthread_mutex_unlock(m_Mutex);
						
						snd_seq_free_event(ev);
    				} while (snd_seq_event_input_pending(seq_handle, 0) > 0);
				}
			}
		}
	}
}

snd_seq_t *MidiDevice::AlsaOpen(Type t) 
{
    snd_seq_t *seq_handle;
    int client_id, port_id;
	
    if (t==WRITE)
	{
    	if (snd_seq_open(&seq_handle, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
    	    fprintf(stderr, "Error opening ALSA sequencer.\n");
    	    exit(1);
    	}
    	snd_seq_set_client_name(seq_handle, m_AppName.c_str());
    	client_id = snd_seq_client_id(seq_handle);
    	if ((port_id = snd_seq_create_simple_port(seq_handle, m_AppName.c_str(),
    	    SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ,
    	    SND_SEQ_PORT_TYPE_APPLICATION) < 0)) {
    	    fprintf(stderr, "Error creating sequencer port.\n");
    	}
	}
	else
	{
		if (snd_seq_open(&seq_handle, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
    	    fprintf(stderr, "Error opening ALSA sequencer.\n");
    	    exit(1);
    	}
    	snd_seq_set_client_name(seq_handle, m_AppName.c_str());
    	client_id = snd_seq_client_id(seq_handle);
    	if ((port_id = snd_seq_create_simple_port(seq_handle, m_AppName.c_str(),
    	    SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE,
    	    SND_SEQ_PORT_TYPE_APPLICATION) < 0)) {
    	    fprintf(stderr, "Error creating sequencer port.\n");
    	}
	}
    return(seq_handle);
}

#endif

#if __APPLE__

void MidiDevice::AppleOpen()
{
	m_ReadFillIndex = m_ReadReadIndex = 0;
	
	OSStatus err = 0;

	mMIDISource					= NULL;
	mMIDIClient					= NULL;
	mMIDIDestination			= NULL;
	
	err = MIDIClientCreate(CFSTR("org.pawpal.ssm"), NULL, NULL, &mMIDIClient);
	if (err) printf("MIDIClientCreate failed returned %d\n", err);
	
	if (!err) {
		err = MIDISourceCreate(mMIDIClient, CFSTR("SpiralSynth"), &mMIDISource);
		if (err) printf("MIDIInputPortCreate failed returned %d\n", err);
	}
	
	if (!err) {
		err = MIDIDestinationCreate(mMIDIClient, CFSTR("SpiralSynth"), sMIDIRead, this, &mMIDIDestination);
		MIDIObjectSetIntegerProperty(mMIDIDestination, kMIDIPropertyUniqueID, 'SSmP');
	}
}


void MidiDevice::AppleClose()
{
	if (mMIDIDestination)
		MIDIEndpointDispose(mMIDIDestination);
	if (mMIDISource)
		MIDIEndpointDispose(mMIDISource);
	mMIDISource = NULL;
	if (mMIDIClient)
		MIDIClientDispose(mMIDIClient);
	mMIDIClient = NULL;
}

int MidiDevice::AppleWrite(int dummy, unsigned char *outbuffer, int maxlen)
{
	return 0;
}

int MidiDevice::AppleRead(int dummy, unsigned char *outbuffer, int maxlen)
{
	if (!mMIDIClient)
		return -1;
	int len = 0;
	do {
		while (m_ReadReadIndex == m_ReadFillIndex)
			usleep(1000);	// 1ms
		int readl =  m_ReadFillIndex - m_ReadReadIndex;
		if (readl < 0)
			readl += midi_ReadSize;	// wrapped
		while (len < maxlen && readl-- > 0) {
			int r = m_ReadReadIndex;
			outbuffer[len++] = m_ReadBuffer[r];
			r++;
			m_ReadReadIndex = r % midi_ReadSize;
		}
	} while (len < maxlen);
	return len;
}

void MidiDevice::sMIDIRead(const MIDIPacketList *pktlist, void *readProcRefCon, void *srcConnRefCon)
{
	MidiDevice & t = *((MidiDevice*)readProcRefCon);
	
	const MIDIPacket *packet = &pktlist->packet[0];
	for (int i = 0; i < (int)pktlist->numPackets; i++) {
		const MIDIPacket & p = *packet;

		for (int b = 0; b < p.length; b++) {
		//	printf("%02x ", p.data[b]);
			int d = t.m_ReadFillIndex;
			t.m_ReadBuffer[d] = p.data[b];
			d++;
			t.m_ReadFillIndex = d % midi_ReadSize;
		}
	//	printf("\n");
		packet = MIDIPacketNext(packet);
	}
}

#endif
