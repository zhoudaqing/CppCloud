#include "iohand.h"
#include "comm/strparse.h"
#include "comm/sock.h"
#include "cloud/const.h"
#include "exception.h"
#include "climanage.h"
#include <sys/epoll.h>
#include <cstring>
#include <cerrno>

HEPMUTICLASS_IMPL(IOHand, IOHand, CliBase)

static map<unsigned, string> s_cmdid2clsname;

// static
int IOHand::Init( void )
{
	// 预定义好的消息->处理类
	s_cmdid2clsname[CMD_WHOAMI_REQ] = "MoniFunc"; // ->BegnHand
	s_cmdid2clsname[CMD_GETCLI_REQ] = "QueryFunc";
	s_cmdid2clsname[CMD_HUNGUP_REQ] = "MoniFunc";
	s_cmdid2clsname[CMD_GETLOGR_REQ] = "QueryFunc";
	s_cmdid2clsname[CMD_EXCHANG_REQ] = "MoniFunc";
	s_cmdid2clsname[CMD_EXCHANG_RSP] = "MoniFunc";
	s_cmdid2clsname[CMD_SETARGS_REQ] = "MoniFunc";
	s_cmdid2clsname[CMD_GETWARN_REQ] = "QueryFunc";
	s_cmdid2clsname[CMD_KEEPALIVE_REQ] = "MoniFunc";
	s_cmdid2clsname[CMD_KEEPALIVE_RSP] = "MoniFunc";

	s_cmdid2clsname[CMD_IAMSERV_REQ] = "RemoteCli"; // 中心端报告身份
	s_cmdid2clsname[CMD_IAMSERV_RSP] = "RemoteCli";

	s_cmdid2clsname[0] = "MoniFunc"; // default handle class

	return 0;
}

IOHand::IOHand(void): m_cliFd(INVALID_FD), m_closeFlag(0),
	  m_iBufItem(NULL), m_oBufItem(NULL)
{
    
}
IOHand::~IOHand(void)
{
	IFDELETE(m_iBufItem);
	IFDELETE(m_oBufItem);
}

int IOHand::onRead( int p1, long p2 )
{
	int ret = 0;
	
	do {
		if (NULL == m_iBufItem)
		{
			m_iBufItem = new IOBuffItem;
		}
		
		if (m_iBufItem->len < HEADER_LEN)
		{
			unsigned rcvlen = 0;
			char buff[HEADER_LEN];
			ret = Sock::recv(m_cliFd, buff, rcvlen, HEADER_LEN);
			IFBREAK_N(ERRSOCK_AGAIN == ret, 0);
			if (ret <= 0) // close 清理流程 
			{
				m_closeReason = (0==ret? "recv closed": strerror(errno));
				throw OffConnException(m_closeReason);
				break;
			}

			m_iBufItem->len += rcvlen;
			if (ret != (int)rcvlen || m_iBufItem->len > HEADER_LEN)
			{
				m_closeFlag = 2;
				m_closeReason = "Sock::recv sys error";
				LOGERROR("IOHAND_READ| msg=recv param error| ret=%d| olen=%u| rcvlen=%u| mi=%s",
				 	 ret, rcvlen, m_iBufItem->len,m_cliName.c_str());
				break;
			}
			
			IFBREAK(m_iBufItem->len < HEADER_LEN); // 头部未完整,wait again
			m_iBufItem->buff.append(buff, m_iBufItem->len); // binary data

			m_iBufItem->ntoh();
			// m_iBufItem->totalLen 合法检查
			head_t* hdr = m_iBufItem->head();
			if (hdr->head_len != HEADER_LEN)
			{
				m_closeFlag = 2;
				m_closeReason = StrParse::Format("headlen invalid(%u)", hdr->head_len);
				break;
			}
			if (hdr->body_len > g_maxpkg_len)
			{
				m_closeFlag = 2;
				m_closeReason = StrParse::Format("bodylen invalid(%u)", hdr->body_len);
				break;
			}
			m_iBufItem->buff.resize(m_iBufItem->totalLen);
		}
		
		// body 接收
		if (m_iBufItem->totalLen > m_iBufItem->len)
		{
			ret = Sock::recv(m_cliFd, (char*)m_iBufItem->head(), m_iBufItem->len, m_iBufItem->totalLen);
			if (ret <= 0)
			{
				m_closeFlag = 2;
				m_closeReason = (0==ret? "recv body closed": strerror(errno));
				break;
			}
		}

		if (m_iBufItem->ioFinish()) // 报文接收完毕
		{
			ret = 0;
			if (m_child)
			{
				ret = Notify(m_child, HEPNTF_NOTIFY_CHILD, (IOBuffItem*)m_iBufItem);
			}
			/// Notify(m_parent, HEPNTF_SET_ALIAS, (IOHand*)this, m_cliName.c_str());
			// parse package [cmdid]
			if (1 == ret)
			{
				ret = cmdProcess(m_iBufItem);
			}

			CliMgr::Instance()->updateCliTime(this);
		}
	}
	while (0);
	return ret;
}

int IOHand::onWrite( int p1, long p2 )
{
	int ret = 0;

	do {
		m_epCtrl.oneShotUpdate();
		if (NULL == m_oBufItem)
		{ // 同步问题... // 
			bool bpopr = m_oBuffq.pop(m_oBufItem, 0);
			WARNLOG_IF1BRK(!bpopr, 0, "IOHAND_WRITE| msg=obuff pop fail| mi=%s", m_cliName.c_str());
		}
		
		ret = Sock::send(m_cliFd, (char*)m_oBufItem->head(), m_oBufItem->len, m_oBufItem->totalLen);
		if (ret <= 0)
		{
			// maybe closed socket
			m_closeFlag = 2;
			m_closeReason = StrParse::Format("send err(%d) sockerr(%s)", ret, strerror(Sock::geterrno(m_cliFd)));
			break;
		}
		
		if (m_oBufItem->len < m_oBufItem->totalLen)
		{
			LOGDEBUG("IOHAND_WRITE| msg=obuf send wait| mi=%s", m_cliName.c_str());
			break;
		}

		IFDELETE(m_oBufItem);
		if (m_oBuffq.size() <= 0)
		{
			ret = m_epCtrl.rmEvt(EPOLLOUT);
			ERRLOG_IF1(ret, "IOHAND_WRITE| msg=rmEvt fail %d| mi=%s| eno=%d", ret, m_cliName.c_str(), errno);
			if (m_oBuffq.size() > 0) // 此处因存在多线程竞争而加此处理
			{
				ret = m_epCtrl.addEvt(EPOLLOUT);
			}

			if (1 == m_closeFlag)
			{
				m_closeFlag = 2;
			}
		}
	}
	while (0);
	return ret;
}

int IOHand::run( int p1, long p2 )
{
	int ret = 0;
	
	try 
	{
		if (EPOLLIN & p1) // 有数据可读
		{
			ret = onRead(p1, p2);
		}
	}
	catch( NormalExceptionOff& exp ) // 业务异常,先发送错误响应,后主动关闭connection
	{
		string rsp = StrParse::Format("{ \"code\": %d, \"desc\": \"%s\" }", exp.code, exp.desc.c_str());
		m_closeFlag = 1; // 待发送完后close
		sendData(exp.cmdid, exp.seqid, rsp.c_str(), rsp.length(), true);
	}
	catch( OffConnException& exp )
	{
		LOGERROR("OffConnException| reson=%s | mi=%s", exp.reson.c_str(), m_idProfile.c_str());
		m_closeFlag = 2;
	}
		
	if (EPOLLOUT & p1) // 可写
	{
		ret = onWrite(p1, p2);
	}


	if ((EPOLLERR|EPOLLHUP) & p1)
	{
		int fderrno = Sock::geterrno(m_cliFd);
		LOGERROR("IOHAND_OTHRE| msg=sock err| mi=%s| err=%d(%s)", m_cliName.c_str(), fderrno, strerror(fderrno));
		m_closeFlag = 2;
	}

	if (HEFG_PEXIT == p1 && 2 == p2) /// #PROG_EXITFLOW(6)
	{
		m_closeFlag = 2; // program exit told
	}

	if (m_closeFlag >= 2)
	{
		ret = onClose(p1, p2);
	}

	return ret;
}

int IOHand::onEvent( int evtype, va_list ap )
{
	int ret = 0;
	if (HEPNTF_INIT_PARAM == evtype)
	{
		int clifd = va_arg(ap, int);
		int epfd = va_arg(ap, int);
		
		m_epCtrl.setEPfd(epfd);
		m_epCtrl.setActFd(clifd);
		m_cliFd = clifd;
		m_cliName = Sock::peer_name(m_cliFd, true);
		m_idProfile = m_cliName;
		ret = m_epCtrl.setEvt(EPOLLIN, this);

		LOGINFO("IOHAND_INIT| msg=a client accept| fd=%d| mi=%s", m_cliFd, m_cliName.c_str());
	}
	else if (HEPNTF_SEND_MSG == evtype)
	{
		unsigned int cmdid = va_arg(ap, unsigned int);
		unsigned int seqid = va_arg(ap, unsigned int);
		const char* body = va_arg(ap, const char*);
		unsigned int bodylen = va_arg(ap, unsigned int);

		ret = sendData(cmdid, seqid, body, bodylen, false);
	}
	else if (HEPNTF_SET_EPOUT == evtype)
	{
		ret = m_epCtrl.addEvt(EPOLLOUT);
		ERRLOG_IF1(ret, "IOHAND_SET_EPOU| msg=set out flag fail %d| mi=%s", ret, m_cliName.c_str());
	}
	else if (HEPNTF_INIT_FINISH == evtype)
	{
		int clity = atoi(m_cliProp[CLIENT_TYPE_KEY].c_str());
		if (clity > 0)
		{
			ERRLOG_IF1RET_N(m_cliType>0&&clity!=m_cliType, -95,
			  "IOHAND_INIT_FIN| msg=clitype not match first| cliType0=%d| cliType1=%d| mi=%s", 
			   m_cliType, clity, m_idProfile.c_str());

			m_idProfile = StrParse::Format("%s@%s-%d@%s", m_cliName.c_str(), 
				m_cliProp[CONNTERID_KEY].c_str(), m_cliType, m_cliProp["name"].c_str());
		}

	}
	else
	{
		ret = -99;
	}
	
	return ret;
}

int IOHand::sendData( unsigned int cmdid, unsigned int seqid, const char* body, unsigned int bodylen, bool setOutAtonce )
{
	IOBuffItem* obf = new IOBuffItem;
	obf->setData(cmdid, seqid, body, bodylen);
	if (!m_oBuffq.append(obf))
	{
		LOGERROR("IOHANDSNDMSG| msg=append to oBuffq fail| len=%d| mi=%s", m_oBuffq.size(), m_cliName.c_str());
		delete obf;
		return -77;
	}

	if (setOutAtonce)
	{
		int ret = m_epCtrl.addEvt(EPOLLOUT);
		ERRLOG_IF1(ret, "IOHAND_SET_EPOU| msg=set out flag fail %d| mi=%s", ret, m_cliName.c_str());
	}

	return 0;
}



void IOHand::setProperty( const string& key, const string& val )
{
	m_cliProp[key] = val;
}

string IOHand::getProperty( const string& key )
{
	map<string, string>::const_iterator itr = m_cliProp.find(key);
	if (itr != m_cliProp.end())
	{
		return itr->second;
	}

	return "";
}

int IOHand::Json2Map( const Value* objnode )
{
	IFRETURN_N(!objnode->IsObject(), -1);
	int ret = 0;
    Value::ConstMemberIterator itr = objnode->MemberBegin();
    for (; itr != objnode->MemberEnd(); ++itr)
    {
        const char* key = itr->name.GetString();
        if (itr->value.IsString())
        {
        	const char* val = itr->value.GetString();
			setProperty(key, val);
        }
		else if (itr->value.IsInt())
		{
			string val = StrParse::Itoa(itr->value.GetInt());
			setProperty(key, val);
		}
    }

    return ret;
}

int IOHand::driveClose( void )
{
	return onClose(0, 0);
}

int IOHand::onClose( int p1, long p2 )
{
	int ret = 0;
	int fderrno = Sock::geterrno(m_cliFd);
	bool isExit = (HEFG_PEXIT == p1 && 2 == p2);

	LOGOPT_EI(ret, "IOHAND_CLOSE| msg=iohand need quit %d| mi=%s| reason=%s| sock=%d%s",
		ret, m_cliName.c_str(), m_closeReason.c_str(), fderrno, fderrno?strerror(fderrno):"");

	IFDELETE(m_child); // 先清理子级对象

	// 清理完监听的evflag
	ret = m_epCtrl.setEvt(0, NULL);
	ERRLOG_IF1(ret, "IOHAND_CLOSE| msg=rm EVflag fail %d| mi=%s| err=%s", ret, m_cliName.c_str(), strerror(errno));

	IFCLOSEFD(m_cliFd);
	ret = Notify(m_parent, HEPNTF_SOCK_CLOSE, this, m_cliType, (int)isExit);
	ERRLOG_IF1(ret, "IOHAND_CLOSE| msg=Notify ret %d| mi=%s| reason=%s", ret, m_cliName.c_str(), m_closeReason.c_str());

	m_closeFlag = 3;
	return ret;
}

int IOHand::cmdProcess( IOBuffItem*& iBufItem )
{
	int ret = 0;
	do 
	{
		IFBREAK_N(NULL==iBufItem, -71);
		head_t* hdr = iBufItem->head();
		string procClsName;

		map<unsigned,string>::iterator it = s_cmdid2clsname.find(hdr->cmdid);
		procClsName = (s_cmdid2clsname.end() != it) ? it->second : s_cmdid2clsname[0];
		WARNLOG_IF1(s_cmdid2clsname.end() == it, "CMDPROCESS| msg=an undefine cmdid recv| cmdid=0x%X| mi=%s", hdr->cmdid, m_cliName.c_str());

		m_cliProp["clisock"] = m_cliName; // 设备ip:port作为其中属性

		/* 首先从cmdid对应到处理类名procClsName,
		   再看该类名有无处理函数ProcessOne(要注册到s_procfunc),用函数处理业备;
		   如果没有就创建处理类对象,用对象处理业务.  */
		HEpBase::ProcOneFunT procFunc = GetProcFunc(procClsName.c_str());
		if (procFunc)
		{
			ret = procFunc(this, hdr->cmdid, (void*)iBufItem);
			IFDELETE(iBufItem);
			break;
		}

		if (m_child) break;
		
		HEpBase* procObj = New(procClsName.c_str());
		if (NULL==procObj)
		{
			LOGERROR("CMDPROCESS| msg=unknow clsname %s| mi=%s", procClsName.c_str(), m_cliName.c_str());
			m_closeReason = "not match process_hand class";
			//m_bClose = true;
			//ret = -72;
			throw NormalExceptionOff(400, hdr->cmdid, hdr->seqid, m_closeReason);
			break;
		}

		Notify(procObj, HEPNTF_INIT_PARAM, this);
		BindSon(this, procObj);
		Notify(m_child, HEPNTF_NOTIFY_CHILD, (IOBuffItem*)m_iBufItem);
	}
	while (0);


	return ret;
}