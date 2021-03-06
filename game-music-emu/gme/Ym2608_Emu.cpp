// Game_Music_Emu $vers. http://www.slack.net/~ant/

#include "Ym2608_Emu.h"
#include "fm.h"
#include <string.h>

static void psg_set_clock(void *param, int clock)
{
	Ym2608_Emu *info = (Ym2608_Emu *)param;
	info->psg_set_clock( clock );
}

static void psg_write(void *param, int address, int data)
{
	Ym2608_Emu *info = (Ym2608_Emu *)param;
	info->psg_write( address, data );
}

static int psg_read(void *param)
{
	Ym2608_Emu *info = (Ym2608_Emu *)param;
	return info->psg_read();
}

static void psg_reset(void *param)
{
	Ym2608_Emu *info = (Ym2608_Emu *)param;
	info->psg_reset();
}

static const ssg_callbacks psgintf =
{
	psg_set_clock,
	psg_write,
	psg_read,
	psg_reset
};

Ym2608_Emu::Ym2608_Emu() { opn = 0; psg.set_type( Ay_Apu::Ym2608 ); }

Ym2608_Emu::~Ym2608_Emu()
{
	if ( opn ) ym2608_shutdown( opn );
}

int Ym2608_Emu::set_rate( int sample_rate, int clock_rate )
{
	if ( opn )
	{
		ym2608_shutdown( opn );
		opn = 0;
	}
	
	opn = ym2608_init( this, clock_rate, sample_rate, &psgintf );
	if ( !opn )
		return 1;

	this->sample_rate = sample_rate;
	psg_clock = clock_rate * 2;

	buffer.set_sample_rate( sample_rate );
	buffer.clock_rate( psg_clock );

	psg.volume( 0.125 );
	
	reset();
	return 0;
}

void Ym2608_Emu::reset()
{
	psg.reset();
	ym2608_reset_chip( opn );
	mute_voices( 0 );
}

static stream_sample_t* DUMMYBUF[0x02] = {(stream_sample_t*)NULL, (stream_sample_t*)NULL};

void Ym2608_Emu::write0( int addr, int data )
{
	ym2608_update_one( opn, DUMMYBUF, 0 );
	ym2608_write( opn, 0, addr );
	ym2608_write( opn, 1, data );
}

void Ym2608_Emu::write1( int addr, int data )
{
	ym2608_update_one( opn, DUMMYBUF, 0 );
	ym2608_write( opn, 2, addr );
	ym2608_write( opn, 3, data );
}

void Ym2608_Emu::write_rom( int rom_id, int size, int start, int length, void * data )
{
	ym2608_write_pcmrom( opn, rom_id, size, start, length, (const UINT8 *) data );
}

void Ym2608_Emu::mute_voices( int mask )
{
	ym2608_set_mutemask( opn, mask );
	for ( unsigned i = 0, j = 1 << 6; i < 3; i++, j <<= 1)
	{
		Blip_Buffer * buf = ( mask & j ) ? NULL : &buffer;
		psg.set_output( i, buf );
	}
}

void Ym2608_Emu::run( int pair_count, sample_t* out )
{
	// buf - destination buffer for PSG
	blip_sample_t buf[ 1024 ];
	// buffers (bufL, bufR) - destination buffers for FM synthesis
	FMSAMPLE bufL[ 1024 ];
	FMSAMPLE bufR[ 1024 ];
	FMSAMPLE * buffers[2] = { bufL, bufR };

	blip_time_t psg_end_time = pair_count * psg_clock / sample_rate;
	psg.end_frame( psg_end_time );

	// buffer - primary BlipBuffer
	buffer.end_frame( psg_end_time );

	while (pair_count > 0)
	{
		int todo = pair_count;
		if (todo > 1024) todo = 1024;

		// render up to 1024 samples to buffers (bufL and bufR)
		ym2608_update_one( opn, buffers, todo );

		// copy BlipBuffer to buf
		int sample_count = buffer.read_samples( buf, todo );
		memset( buf + sample_count, 0, ( todo - sample_count ) * sizeof( blip_sample_t ) );

		for (int i = 0; i < todo; i++) {
			out[0] = short((buf[i] + bufL[i]) * 0.75);
			out[1] = short((buf[i] + bufR[i]) * 0.75);
			out += 2;
		}

		pair_count -= todo;
	}
}

void Ym2608_Emu::psg_set_clock( int clock )
{
    psg_clock = clock * 2;
	buffer.clock_rate( psg_clock );
}

void Ym2608_Emu::psg_write( int addr, int data )
{
    if ( !(addr & 1) ) psg.write_addr( data );
    else psg.write_data( 0, data );
}

int Ym2608_Emu::psg_read()
{
	return psg.read();
}

void Ym2608_Emu::psg_reset()
{
	psg.reset();
}
