#include <stdarg.h>
#include <string>
#include <Windows.h>

//#include <QDateTime>
//#include <QDir>

static FILE* log_fp = nullptr;

std::string format_time()
{
	char time_buf[1024];

	SYSTEMTIME time;
	::GetLocalTime(&time);

	sprintf_s(time_buf, 1024,
		"%04d-%02d-%02d %02d:%02d:%02d.%03d",
		time.wYear, time.wMonth, time.wDay,
		time.wHour, time.wMinute, time.wSecond, time.wMilliseconds
	);

	return time_buf;
}

std::string get_app_dir()
{
    char Path[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, Path, MAX_PATH);//
    char*pChar = strrchr(Path, '\\');
    if (pChar) {
        *(pChar + 1) = 0;
    }
    return std::string(Path);
}

void init_log()
{
	if (nullptr == log_fp) 
	{
		//QString time = QDateTime::currentDateTime().toString("yyyy-MM-dd-hh-mm-ss");
  //      std::string filePath = get_app_dir();
  //      filePath += "player";
		//filePath += time.toStdString();
		//filePath += ".log";
		//QDir tempPath = QDir::temp().absoluteFilePath("WISROOM");
		//QString time = QDateTime::currentDateTime().toString("yyyy-MM-dd-hh-mm-ss");
		//std::string logname = "player" + time.toStdString() + ".log";
		//QString path = tempPath.absoluteFilePath(QString::fromStdString(logname));
		//log_fp = fopen(path.toStdString().c_str(), "w");
		//if (nullptr == log_fp) 
		//{
		//	printf("open log file fail\n");
		//}
	}
}

void player_log(std::string& fun, const char *fmt, ...)
{
	va_list ap;
	int n = 0;
	char str_buff[2048] = {0};
	va_start(ap, fmt); //获得可变参数列表,分析fmt，得出各个参数的类型，也是分析参数中类似”%d%s“的字符串
	n = vsnprintf(str_buff, 2048, fmt, ap); //写入字符串s
	va_end(ap); //释放资源,必须与va_start成对调用

	printf("%s\n", str_buff);
	if (log_fp)
	{
		//QString time = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss:zzz");
		//fprintf(log_fp, "[%s][%s] %s\n", time.toStdString().c_str(), fun.c_str(), str_buff);
		//fflush(log_fp);
	}
		
}

void ffmpeg_log_callback(void* ptr, int level, const char* fmt, va_list vl)
{
	if (level <= 32) {
		char buff[4096];
		vsnprintf(buff, 4096, fmt, vl);

		//if (log_fp) {
			fprintf(stdout, "[ffmpeg] %d %s", level, buff);
			//fflush(log_fp);
		//}
	}
}
