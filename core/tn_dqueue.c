/*

  TNKernel real-time kernel

  Copyright � 2004, 2013 Yuri Tiomkin
  All rights reserved.

  Permission to use, copy, modify, and distribute this software in source
  and binary forms and its documentation for any purpose and without fee
  is hereby granted, provided that the above copyright notice appear
  in all copies and that both that copyright notice and this permission
  notice appear in supporting documentation.

  THIS SOFTWARE IS PROVIDED BY THE YURI TIOMKIN AND CONTRIBUTORS "AS IS" AND
  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL YURI TIOMKIN OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
  OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
  SUCH DAMAGE.

*/

  /* ver 2.7  */

#include "tn_common.h"
#include "tn_sys.h"
#include "tn_internal.h"

#include "tn_dqueue.h"

#include "tn_tasks.h"


static enum TN_Retval  dque_fifo_write(struct TN_DQueue * dque, void * data_ptr);
static enum TN_Retval  dque_fifo_read(struct TN_DQueue * dque, void ** data_ptr);

//-------------------------------------------------------------------------
// Structure's field dque->id_dque have to be set to 0
//-------------------------------------------------------------------------
enum TN_Retval tn_queue_create(
      struct TN_DQueue *dque,      //-- Ptr to already existing struct TN_DQueue
      void **data_fifo,   //-- Ptr to already existing array of void * to store data queue entries.
                          //   Can be NULL.
      int num_entries     //-- Capacity of data queue(num entries). Can be 0
      )
{
#if TN_CHECK_PARAM
   if(dque == NULL)
      return TERR_WRONG_PARAM;
   if(num_entries < 0 || dque->id_dque == TN_ID_DATAQUEUE)
      return TERR_WRONG_PARAM;
#endif

   tn_list_reset(&(dque->wait_send_list));
   tn_list_reset(&(dque->wait_receive_list));

   dque->data_fifo      = data_fifo;
   dque->num_entries    = num_entries;
   if (dque->data_fifo == NULL){
      dque->num_entries = 0;
   }

   dque->tail_cnt   = 0;
   dque->header_cnt = 0;

   dque->id_dque = TN_ID_DATAQUEUE;

   return TERR_NO_ERR;
}

//----------------------------------------------------------------------------
enum TN_Retval tn_queue_delete(struct TN_DQueue * dque)
{
   TN_INTSAVE_DATA
   struct TN_ListItem * que;
   struct TN_Task * task;

#if TN_CHECK_PARAM
   if(dque == NULL)
      return TERR_WRONG_PARAM;
   if(dque->id_dque != TN_ID_DATAQUEUE)
      return TERR_NOEXS;
#endif

   TN_CHECK_NON_INT_CONTEXT

   tn_disable_interrupt(); // v.2.7 - thanks to Eugene Scopal

   while (!tn_is_list_empty(&(dque->wait_send_list))){

     //--- delete from sem wait queue

      que = tn_list_remove_head(&(dque->wait_send_list));
      task = get_task_by_tsk_queue(que);

      _tn_task_wait_complete(task, (0));
      task->task_wait_rc = TERR_DLT;

      if (_tn_need_context_switch()){
         tn_enable_interrupt();
         tn_switch_context();
         tn_disable_interrupt();
      }
   }

   while (!tn_is_list_empty(&(dque->wait_receive_list))){
     //--- delete from sem wait queue

      que = tn_list_remove_head(&(dque->wait_receive_list));
      task = get_task_by_tsk_queue(que);

      _tn_task_wait_complete(task, (0));
      task->task_wait_rc = TERR_DLT;

      if (_tn_need_context_switch()){
         tn_enable_interrupt();
         tn_switch_context();
         tn_disable_interrupt();
      }
   }
      
   dque->id_dque = 0; // Data queue not exists now

   tn_enable_interrupt();

   return TERR_NO_ERR;

}

//----------------------------------------------------------------------------
enum TN_Retval tn_queue_send(struct TN_DQueue * dque, void * data_ptr, unsigned long timeout)
{
   TN_INTSAVE_DATA;
   enum TN_Retval rc = TERR_NO_ERR;
   struct TN_ListItem * que;
   struct TN_Task * task;
   BOOL bool_wait = FALSE;

#if TN_CHECK_PARAM
   if(dque == NULL || timeout == 0)
      return  TERR_WRONG_PARAM;
   if(dque->id_dque != TN_ID_DATAQUEUE)
      return TERR_NOEXS;
#endif

   TN_CHECK_NON_INT_CONTEXT;

   tn_disable_interrupt();

  //-- there are task(s) in the data queue's wait_receive list

   if (!tn_is_list_empty(&(dque->wait_receive_list))){
      que  = tn_list_remove_head(&(dque->wait_receive_list));
      task = get_task_by_tsk_queue(que);

      task->data_elem = data_ptr;
      _tn_task_wait_complete(task, (0));

   } else  {
      //-- the data queue's  wait_receive list is empty
      rc = dque_fifo_write(dque,data_ptr);
      if (rc != TERR_NO_ERR){
         //-- no free entries in the data queue
         tn_curr_run_task->data_elem = data_ptr;  //-- Store data_ptr
         _tn_task_curr_to_wait_action(
               &(dque->wait_send_list),
               TSK_WAIT_REASON_DQUE_WSEND,
               timeout
               );
         bool_wait = TRUE;
      }
   }

#if TN_DEBUG
   if (!_tn_need_context_switch() && bool_wait){
      TN_FATAL_ERROR("");
   }
#endif

   tn_enable_interrupt();
   _tn_switch_context_if_needed();
   if (bool_wait){
      rc = tn_curr_run_task->task_wait_rc;
   }
   return rc;
}

//----------------------------------------------------------------------------
enum TN_Retval tn_queue_send_polling(struct TN_DQueue * dque, void * data_ptr)
{
   TN_INTSAVE_DATA;
   enum TN_Retval rc = TERR_NO_ERR;
   struct TN_ListItem * que;
   struct TN_Task * task;

#if TN_CHECK_PARAM
   if(dque == NULL)
      return  TERR_WRONG_PARAM;
   if(dque->id_dque != TN_ID_DATAQUEUE)
      return TERR_NOEXS;
#endif

   TN_CHECK_NON_INT_CONTEXT;

   tn_disable_interrupt();

  //-- there are task(s) in the data queue's  wait_receive list

   if (!tn_is_list_empty(&(dque->wait_receive_list))){
      que  = tn_list_remove_head(&(dque->wait_receive_list));
      task = get_task_by_tsk_queue(que);

      task->data_elem = data_ptr;

      _tn_task_wait_complete(task, (0));
   } else {
      //-- the data queue's  wait_receive list is empty
      rc = dque_fifo_write(dque,data_ptr);
      if (rc != TERR_NO_ERR){
         //-- No free entries in data queue
         rc = TERR_TIMEOUT;  //-- Just convert errorcode
      }
   }
   tn_enable_interrupt();
   _tn_switch_context_if_needed();

   return rc;
}

//----------------------------------------------------------------------------
enum TN_Retval tn_queue_isend_polling(struct TN_DQueue * dque, void * data_ptr)
{
   TN_INTSAVE_DATA_INT;
   enum TN_Retval rc = TERR_NO_ERR;
   struct TN_ListItem * que;
   struct TN_Task * task;

#if TN_CHECK_PARAM
   if(dque == NULL)
      return  TERR_WRONG_PARAM;
   if(dque->id_dque != TN_ID_DATAQUEUE)
      return TERR_NOEXS;
#endif

   TN_CHECK_INT_CONTEXT;

   tn_idisable_interrupt();

  //-- there are task(s) in the data queue's  wait_receive list

   if (!tn_is_list_empty(&(dque->wait_receive_list))){
      que  = tn_list_remove_head(&(dque->wait_receive_list));
      task = get_task_by_tsk_queue(que);

      task->data_elem = data_ptr;

      _tn_task_wait_complete(task, (0));
   } else {
      //-- the data queue's wait_receive list is empty
      rc = dque_fifo_write(dque,data_ptr);

      if (rc != TERR_NO_ERR){
         //-- No free entries in data queue
         rc = TERR_TIMEOUT;  //-- Just convert errorcode
      }
   }

   tn_ienable_interrupt();

   return rc;
}

//----------------------------------------------------------------------------
enum TN_Retval tn_queue_receive(struct TN_DQueue * dque,void ** data_ptr,unsigned long timeout)
{
   TN_INTSAVE_DATA;
   enum TN_Retval rc = TERR_NO_ERR; //-- return code
   struct TN_ListItem * que;
   struct TN_Task * task;
   BOOL waited_for_data = FALSE;

#if TN_CHECK_PARAM
   if(dque == NULL || timeout == 0 || data_ptr == NULL)
      return  TERR_WRONG_PARAM;
   if(dque->id_dque != TN_ID_DATAQUEUE)
      return TERR_NOEXS;
#endif

   TN_CHECK_NON_INT_CONTEXT;

   tn_disable_interrupt();

   rc = dque_fifo_read(dque,data_ptr);
   if (rc == TERR_NO_ERR){
      //-- There was entry(s) in data queue
      if (!tn_is_list_empty(&(dque->wait_send_list))){
         que  = tn_list_remove_head(&(dque->wait_send_list));
         task = get_task_by_tsk_queue(que);

         dque_fifo_write(dque,task->data_elem); //-- Put to data FIFO

         _tn_task_wait_complete(task, (0));
      }
   } else {
      //-- data FIFO is empty
      if (!tn_is_list_empty(&(dque->wait_send_list))){
         que  = tn_list_remove_head(&(dque->wait_send_list));
         task = get_task_by_tsk_queue(que);

         *data_ptr = task->data_elem; //-- Return to caller
         _tn_task_wait_complete(task, (0));

      } else {
         //-- wait_send_list is empty
         _tn_task_curr_to_wait_action(&(dque->wait_receive_list),
                                     TSK_WAIT_REASON_DQUE_WRECEIVE,timeout);

         waited_for_data = TRUE;

      }
   }

#if TN_DEBUG
   if (!_tn_need_context_switch() && waited_for_data){
      TN_FATAL_ERROR("");
   }
#endif


   tn_enable_interrupt();
   _tn_switch_context_if_needed();
   if (waited_for_data){
      //-- When returns to this point, in the data_elem have to be valid value

      *data_ptr = tn_curr_run_task->data_elem; //-- Return to caller
      rc = tn_curr_run_task->task_wait_rc;
   }

   return rc;
}

//----------------------------------------------------------------------------
enum TN_Retval tn_queue_receive_polling(struct TN_DQueue * dque,void ** data_ptr)
{
   TN_INTSAVE_DATA;
   enum TN_Retval rc = TERR_NO_ERR;
   struct TN_ListItem * que;
   struct TN_Task * task;

#if TN_CHECK_PARAM
   if(dque == NULL || data_ptr == NULL)
      return  TERR_WRONG_PARAM;
   if(dque->id_dque != TN_ID_DATAQUEUE)
      return TERR_NOEXS;
#endif

   TN_CHECK_NON_INT_CONTEXT;

   tn_disable_interrupt();

   rc = dque_fifo_read(dque,data_ptr);
   if (rc == TERR_NO_ERR){
      //-- There was entry(s) in data queue
      if (!tn_is_list_empty(&(dque->wait_send_list))){
         que  = tn_list_remove_head(&(dque->wait_send_list));
         task = get_task_by_tsk_queue(que);

         dque_fifo_write(dque,task->data_elem); //-- Put to data FIFO
         _tn_task_wait_complete(task, (0));
      }
   } else {
      //-- data FIFO is empty
      if (!tn_is_list_empty(&(dque->wait_send_list))){
         que  = tn_list_remove_head(&(dque->wait_send_list));
         task = get_task_by_tsk_queue(que);

         *data_ptr = task->data_elem; //-- Return to caller
         _tn_task_wait_complete(task, (0));
      } else {
         //-- wait_send_list is empty
         rc = TERR_TIMEOUT;
      }

   }

   tn_enable_interrupt();
   _tn_switch_context_if_needed();

   return rc;
}

//----------------------------------------------------------------------------
enum TN_Retval tn_queue_ireceive(struct TN_DQueue * dque,void ** data_ptr)
{
   TN_INTSAVE_DATA_INT;
   enum TN_Retval rc = TERR_NO_ERR;
   struct TN_ListItem * que;
   struct TN_Task * task;

#if TN_CHECK_PARAM
   if(dque == NULL || data_ptr == NULL)
      return  TERR_WRONG_PARAM;
   if(dque->id_dque != TN_ID_DATAQUEUE)
      return TERR_NOEXS;
#endif

   TN_CHECK_INT_CONTEXT;

   tn_idisable_interrupt();

   rc = dque_fifo_read(dque,data_ptr);
   if (rc == TERR_NO_ERR){
      //-- There was entry(s) in data queue
      if (!tn_is_list_empty(&(dque->wait_send_list))){
         que  = tn_list_remove_head(&(dque->wait_send_list));
         task = get_task_by_tsk_queue(que);

         dque_fifo_write(dque,task->data_elem); //-- Put to data FIFO

         _tn_task_wait_complete(task, (0));
      }
   } else {
      //-- data FIFO is empty
      if (!tn_is_list_empty(&(dque->wait_send_list))){
         que  = tn_list_remove_head(&(dque->wait_send_list));
         task =  get_task_by_tsk_queue(que);

         *data_ptr = task->data_elem; //-- Return to caller

         _tn_task_wait_complete(task, (0));

         rc = TERR_NO_ERR;
      } else {
         rc = TERR_TIMEOUT;
      }
   }

   tn_ienable_interrupt();

   return rc;
}



//---------------------------------------------------------------------------
//    Data queue storage FIFO processing
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
static enum TN_Retval  dque_fifo_write(struct TN_DQueue * dque, void * data_ptr)
{
   register int flag;

#if TN_CHECK_PARAM
   if(dque == NULL)
      return TERR_WRONG_PARAM;
#endif

   //-- v.2.7

   if(dque->num_entries <= 0)
      return TERR_OUT_OF_MEM;

   flag = ((dque->tail_cnt == 0 && dque->header_cnt == dque->num_entries - 1)
         || dque->header_cnt == dque->tail_cnt-1);
   if(flag)
      return  TERR_OVERFLOW;  //--  full

   //-- wr  data

   dque->data_fifo[dque->header_cnt] = data_ptr;
   dque->header_cnt++;
   if(dque->header_cnt >= dque->num_entries)
      dque->header_cnt = 0;
   return TERR_NO_ERR;
}

//----------------------------------------------------------------------------
static enum TN_Retval  dque_fifo_read(struct TN_DQueue * dque, void ** data_ptr)
{

#if TN_CHECK_PARAM
   if(dque == NULL || data_ptr == NULL)
      return TERR_WRONG_PARAM;
#endif

   //-- v.2.7  Thanks to kosyak� from electronix.ru

   if(dque->num_entries <= 0)
      return TERR_OUT_OF_MEM;

   if(dque->tail_cnt == dque->header_cnt)
      return TERR_UNDERFLOW; //-- empty

   //-- rd data

   *data_ptr  =  dque->data_fifo[dque->tail_cnt];
   dque->tail_cnt++;
   if(dque->tail_cnt >= dque->num_entries)
      dque->tail_cnt = 0;

   return TERR_NO_ERR;
}




//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------


