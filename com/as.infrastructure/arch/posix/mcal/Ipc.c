/**
 * AS - the open source Automotive Software on https://github.com/parai
 *
 * Copyright (C) 2015  AS <parai@foxmail.com>
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation; See <http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
/* ============================ [ INCLUDES  ] ====================================================== */
#ifdef __WINDOWS__
#include <windows.h>
#endif
#include <pthread.h>
#include <unistd.h>
#include "Ipc.h"
#include "asdebug.h"
/* ============================ [ MACROS    ] ====================================================== */
#define AS_LOG_IPC 0
/* ============================ [ TYPES     ] ====================================================== */
typedef struct
{
	uint32 r_pos;
	uint32 w_pos;
	void* thread;
	boolean ready;
}Ipc_ChannelRuntimeType;

typedef struct
{
	const Ipc_ConfigType* config;
	Ipc_ChannelRuntimeType runtime[IPC_CHL_NUM];
	boolean bInitialized;
}ipc_t;
/* ============================ [ DECLARES  ] ====================================================== */
/* ============================ [ DATAS     ] ====================================================== */
static ipc_t ipc =
{
	.bInitialized = FALSE,
};
/* ============================ [ LOCALS    ] ====================================================== */
static boolean fifo_read(Ipc_ChannelRuntimeType* runtime, Ipc_ChannelConfigType* config, VirtQ_IdxType *idx)
{
    boolean ercd;
    if(config->r_fifo->count > 0)
    {
    	//asmem(config->r_fifo,512);
        *idx = config->r_fifo->idx[runtime->r_pos];
        ASLOG(IPC,"Incoming message: 0x%X,pos=%d,count=%d from thread=%08X\n",*idx,runtime->r_pos,config->r_fifo->count,pthread_self());
        config->r_fifo->count -= 1;
        runtime->r_pos = (runtime->r_pos + 1)%(IPC_FIFO_SIZE);
        ercd = TRUE;
    }
    else
    {
        ercd  = FALSE;
    }
    return ercd;
}
static bool fifo_write(Ipc_ChannelRuntimeType* runtime, Ipc_ChannelConfigType* config, VirtQ_IdxType idx)
{
	bool ercd;
#ifdef __WINDOWS__
	WaitForSingleObject(config->w_lock,INFINITE);
#else
	(void)pthread_mutex_lock((pthread_mutex_t *)config->w_lock);
#endif
	if(config->w_fifo->count < IPC_FIFO_SIZE)
	{
		config->w_fifo->idx[runtime->w_pos] = idx;
		config->w_fifo->count += 1;
		ASLOG(IPC,"Transmit message: 0x%X,pos=%d,count=%d from thread=%08X\n",idx,runtime->w_pos,config->w_fifo->count,pthread_self());
		runtime->w_pos = (runtime->w_pos + 1)%(IPC_FIFO_SIZE);
		ercd = true;
	}
	else
	{
		assert(0);
		ercd = false;
	}
#ifdef __WINDOWS__
	ReleaseMutex(config->w_lock);
	SetEvent( config->w_event );
#else
	(void)pthread_mutex_unlock( (pthread_mutex_t *)config->w_lock );
	(void)pthread_cond_signal ((pthread_cond_t *)config->w_event);
	usleep(1);
#endif
	return ercd;
}
#ifdef __WINDOWS__
static DWORD Ipc_Daemon(PVOID lpParameter)
#else
static void* Ipc_Daemon(void* lpParameter)
#endif
{
#ifdef __WINDOWS__
	HANDLE pvObjectList[ 2 ];
#endif
	VirtQ_IdxType idx;
	uint32 i;
	boolean ercd;
	Ipc_ChannelType chl;
	Ipc_ChannelConfigType* config;
	Ipc_ChannelRuntimeType* runtime;
	chl = (Ipc_ChannelType)(unsigned long)lpParameter;
	assert(chl<IPC_CHL_NUM);
	config = &(ipc.config->channelConfig[chl]);
	runtime = &(ipc.runtime[chl]);
#ifdef __WINDOWS__
	pvObjectList[ 0 ] = config->r_lock;
	pvObjectList[ 1 ] = config->r_event;
#endif
	while(NULL == config->r_lock)
	{
		usleep(1);
	}
    ASLOG(OFF,"r_lock=%08X, w_lock=%08X, r_event=%08X, w_event=%08X, r_fifo=%08X, w_fifo=%08X\n",
          config->r_lock,config->w_lock,config->r_event,config->w_event,config->r_fifo,config->w_fifo);
	runtime->ready = TRUE;
	while(true)
	{
#ifdef __WINDOWS__
		WaitForMultipleObjects( sizeof( pvObjectList ) / sizeof( HANDLE ), pvObjectList, TRUE, INFINITE );
#else
		(void)pthread_cond_wait ((pthread_cond_t *)config->r_event,(pthread_mutex_t *)config->r_lock);
#endif
		do {
			ercd = fifo_read(runtime,config,&idx);
			if(ercd)
			{
				for( i=0 ; i<config->map_size; i++)
				{
					if(config->mapping->idx == idx)
					{
						assert(config->rxNotification);
						config->rxNotification(config->mapping->chl);
						break;
					}
				}
				assert(i<config->map_size);
			}
			else
			{
				/* do nothing as empty */
			}
		}while(ercd);
#ifdef __WINDOWS__
		ReleaseMutex( config->r_lock );
#else
		(void)pthread_mutex_unlock( (pthread_mutex_t *)config->r_lock );
#endif
	}
	return 0;
}
boolean Ipc_IsReady(Ipc_ChannelType chl)
{
	boolean status = FALSE;
	if(chl<IPC_CHL_NUM)
	{
		status = ipc.runtime[chl].ready;
	}
	else
	{
		assert(0);
	}
	return status;
}
/* ============================ [ FUNCTIONS ] ====================================================== */
void Ipc_Init(const Ipc_ConfigType* config)
{
	Ipc_ChannelType i;
	if(FALSE == ipc.bInitialized)
	{
		ipc.bInitialized = TRUE;

		ipc.config = config;

		for(i=0;i<IPC_CHL_NUM;i++)
		{
			#ifdef __WINDOWS__
			ipc.runtime[i].thread = CreateThread( NULL, 0, Ipc_Daemon, (void*)i, 0, NULL );
			#else
			pthread_create((pthread_t*)&(ipc.runtime[i].thread),NULL,Ipc_Daemon,(void*)(unsigned long)i);
			#endif
			ipc.runtime[i].r_pos = 0;
			ipc.runtime[i].w_pos = 0;
			ipc.runtime[i].ready = FALSE;
		}
	}
	else
	{
		assert(0);
	}
}
void Ipc_WriteIdx(Ipc_ChannelType chl, uint16 idx)
{
	Ipc_ChannelConfigType* config;
	Ipc_ChannelRuntimeType* runtime;

	assert(chl<IPC_CHL_NUM);
	config = &(ipc.config->channelConfig[chl]);
	runtime = &(ipc.runtime[chl]);

	fifo_write(runtime,config,idx);
}
