#ifndef __LOG_H__
#define __LOG_H__

void log_Init(char *filename);
void log_Done();
void log_Print(char *format, ...);

#endif
