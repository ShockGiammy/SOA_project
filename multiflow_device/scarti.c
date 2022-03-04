
int re_write_buffer(char *buffer, size_t off, size_t len);
bool test_float(const char *str);

// in dev_read
// potresti lavorare in buffer circolare => servono due indici (offset e validBytes)
   /*ret = re_write_buffer(the_object->stream_content[the_object->priority], the_object->offset, len);
   if (ret != 0) {
      printk("Error in re_write_buffer");
   }*/




bool test_float(const char *str) {
    int len;
    float dummy = 0.0;
    if (sscanf(str, "%f %n", &dummy, &len) == 1 && len == (int)strlen(str))
        //printf("[%s] is valid (%.7g)\n", str, dummy);
        return true;
    else
        //printf("[%s] is not valid (%.7g)\n", str, dummy);
        return false;
}


int re_write_buffer(char *buffer, size_t off, size_t len) {

   int i;
   if (len != 0) {
      for (i = 0; i < OBJECT_MAX_SIZE - len; i++) {
         buffer[i] = buffer[len+i];
      }
   }
   return 0;
}

