/**
 * Appcelerator Titanium - licensed under the Apache Public License 2
 * see LICENSE in the root folder for details on the license.
 * Copyright (c) 2009 Appcelerator, Inc. All Rights Reserved.
 */

#include "input_pipe.h"
#include "buffered_input_pipe.h"

#if defined(OS_OSX)
# include "osx/osx_input_pipe.h"
#elif defined(OS_WIN32)
# include "win32/win32_input_pipe.h"
#elif defined(OS_LINUX)
# include "linux/linux_input_pipe.h"
#endif

namespace ti
{
	/*static*/
	AutoInputPipe InputPipe::CreateInputPipe()
	{
		
#if defined(OS_OSX)
		AutoInputPipe pipe = new OSXInputPipe();
#elif defined(OS_WIN32)
		AutoInputPipe pipe = new Win32InputPipe();
#elif defined(OS_LINUX)
		AutoInputPipe pipe = new LinuxInputPipe();
#endif
		return pipe;
	}
	
	InputPipe::InputPipe() :
		Pipe("Process.InputPipe"),
		onRead(0),
		onClose(0),
		joined(false),
		joinedRead(0),
		attachedOutput(0),
		asyncOnRead(true)
	{
		logger = Logger::Get("Process.InputPipe");
		
		//TODO doc me
		SetMethod("read", &InputPipe::_Read);
		SetMethod("readLine", &InputPipe::_ReadLine);
		SetMethod("join", &InputPipe::_Join);
		SetMethod("unjoin", &InputPipe::_Unjoin);
		SetMethod("isJoined", &InputPipe::_IsJoined);
		SetMethod("split", &InputPipe::_Split);
		SetMethod("unsplit", &InputPipe::_Unsplit);
		SetMethod("isSplit", &InputPipe::_IsSplit);
		SetMethod("attach", &InputPipe::_Attach);
		SetMethod("detach", &InputPipe::_Detach);
		SetMethod("isAttached", &InputPipe::_IsAttached);
		SetMethod("isPiped", &InputPipe::_IsPiped);
		SetMethod("setOnRead", &InputPipe::_SetOnRead);
		SetMethod("setOnClose", &InputPipe::_SetOnClose);
	}
	
	InputPipe::~InputPipe()
	{
	}
	
	void InputPipe::DataReady(AutoInputPipe pipe)
	{
		if (pipe.isNull()) {
			this->duplicate();
			pipe = this;
		}
		
		if (IsAttached())
		{
			if (!(*attachedOutput)->GetMethod("write").isNull())
			{
				AutoPtr<Blob> data = pipe->Read();
				SharedValue result = (*attachedOutput)->CallNS("write", Value::NewObject(data));
			}
		}
		else
		{
			if (onRead != NULL && !onRead->isNull())
			{
				ValueList args;
				SharedKObject event = new StaticBoundObject();
				event->SetObject("pipe", pipe);
				
				args.push_back(Value::NewObject(event));
				try
				{
					logger->Debug("invoke method on main thread, asyncOnRead?%s",asyncOnRead?"true":"false");
					Host::GetInstance()->InvokeMethodOnMainThread(*this->onRead, args, !asyncOnRead);
				}
				catch (ValueException& e)
				{
					logger->Error(e.DisplayString()->c_str());
				}
			}
		}
	}
	
	void InputPipe::Closed()
	{
		//Logger::Get("Process.InputPipe")->Debug("in InputPipe::Closed");
		if (IsAttached())
		{
			//Logger::Get("Process.InputPipe")->Debug("I'm attached");
			if (!(*attachedOutput)->GetMethod("close").isNull())
			{
				SharedKMethod method = (*attachedOutput)->GetMethod("close");
				ValueList args;
				Host::GetInstance()->InvokeMethodOnMainThread(method, args, false);
			}
		}
		else if (IsSplit())
		{
			splitPipe1->Close();
			splitPipe2->Close();
		}
		else if (IsJoined())
		{
			this->duplicate();
			AutoInputPipe autoThis = this;
			joinedTo->Unjoin(autoThis);
		}
		
		if (onClose && !onClose->isNull())
		{
			ValueList args;
			SharedKObject event = new StaticBoundObject();
			this->duplicate();
			AutoPtr<InputPipe> autoThis = this;
			event->SetObject("pipe", autoThis);
			
			args.push_back(Value::NewObject(event));
			
			try
			{
				Host::GetInstance()->InvokeMethodOnMainThread(*this->onClose, args, false);
			}
			catch (ValueException& e)
			{
				logger->Error(e.DisplayString()->c_str());
			}
		}
	}
	
	void InputPipe::Join(AutoInputPipe other)
	{
		joinedPipes.push_back(other);
		other->joined = true;
		this->duplicate();
		other->joinedTo = this;
		
		if (joinedRead.isNull())
		{
	 		MethodCallback* joinedCallback =
				NewCallback<InputPipe, const ValueList&, SharedValue>(this, &InputPipe::JoinedRead);
			
			joinedRead = new StaticBoundMethod(joinedCallback);
		}
		
		other->onRead = new SharedKMethod(joinedRead);
	}
	
	void InputPipe::JoinedRead(const ValueList& args, SharedValue result)
	{
		AutoInputPipe pipe = args.at(0)->ToObject().cast<InputPipe>();
		pipe->duplicate();
		DataReady(pipe);
	}
	
	void InputPipe::Unjoin(AutoInputPipe other)
	{
		if (other->joinedTo.get() == this)
		{
			other->joined = false;
			other->onRead = NULL;
			other->joinedTo = NULL;
		}
	}
	
	bool InputPipe::IsJoined()
	{
		return this->joined;
	}
	
	void InputPipe::Split()
	{
		Poco::Mutex::ScopedLock lock(splitMutex);
		if (IsSplit())
		{
			return;
		}
		
		splitPipe1 = new BufferedInputPipe();
		splitPipe2 = new BufferedInputPipe();
		
		MethodCallback* splitCallback = NewCallback<InputPipe, const ValueList&, SharedValue>(this, &InputPipe::Splitter);
		MethodCallback* closeCallback = NewCallback<InputPipe, const ValueList&, SharedValue>(this, &InputPipe::SplitterClose);
		this->onRead = new SharedKMethod(new StaticBoundMethod(splitCallback));
		this->onClose = new SharedKMethod(new StaticBoundMethod(closeCallback));
	}
	
	void InputPipe::Splitter(const ValueList& args, SharedValue result)
	{
		Poco::Mutex::ScopedLock lock(splitMutex);
		
		AutoPtr<Blob> blob = this->Read();
		splitPipe1->Append(blob);
		splitPipe2->Append(blob);
	}
	
	void InputPipe::SplitterClose(const ValueList& args, SharedValue result)
	{
		Poco::Mutex::ScopedLock lock(splitMutex);
		
		splitPipe1->Close();
		splitPipe2->Close();
	}
	
	void InputPipe::Unsplit()
	{
		if (IsSplit())
		{
			Poco::Mutex::ScopedLock lock(splitMutex);
		
			delete this->onRead;
			delete this->onClose;
			
			splitPipe1->Close();
			splitPipe2->Close();
			splitPipe1 = NULL;
			splitPipe2 = NULL;
		}
	}
	
	bool InputPipe::IsSplit()
	{
		return !splitPipe1.isNull();
	}
	
	void InputPipe::Attach(SharedKObject other)
	{
		attachedOutput = new SharedKObject(other);
		Logger::Get("Process.InputPipe")->Debug("attaching object.. hasWrite? %s, hasClose? %s",
			!(*attachedOutput)->Get("write")->IsUndefined()?"TRUE":"FALSE",
			!(*attachedOutput)->Get("close")->IsUndefined()?"TRUE":"FALSE");
	}
	
	void InputPipe::Detach()
	{
		if (attachedOutput && !attachedOutput->isNull())
		{
			delete attachedOutput;
		}
	}
	
	bool InputPipe::IsAttached()
	{
		return attachedOutput != NULL && !attachedOutput->isNull();
	}
	
	void InputPipe::SetOnRead(SharedKMethod onReadCallback)
	{
		if (this->onRead && onReadCallback.get() == this->onRead->get()) return;
		
		if (IsJoined())
		{
			this->duplicate();
			AutoInputPipe autoThis = this;
			joinedTo->Unjoin(autoThis);
		}
		else if (IsSplit())
		{
			Unsplit();
		}
		else if (IsAttached())
		{
			Detach();
		}
		
		if (!onReadCallback.isNull())
		{
			this->onRead = new SharedKMethod(onReadCallback);
		}
		else
		{
			this->onRead = NULL;
		}
	}
	
	int InputPipe::FindFirstLineFeed(char *data, int length, int *charsToErase)
	{
		int newline = -1;
		for (int i = 0; i < length; i++)
		{
			if (data[i] == '\n')
			{
				newline = i;
				*charsToErase = 1;
				break;
			}
			else if (data[i] == '\r')
			{
				if (i < length-1 && data[i+1] == '\n')
				{
					newline = i+1;
					*charsToErase = 2;
					break;
				}
			}
		}
		
		return newline;
	}
	
	AutoInputPipe InputPipe::Clone()
	{
		AutoInputPipe pipe = CreateInputPipe();
		if (onRead && !onRead->isNull())
		{
			pipe->SetOnRead(*onRead);
		}
		if (onClose && !onClose->isNull())
		{
			pipe->onClose = new SharedKMethod(*onClose);
		}
		
		return pipe;
	}
	
	void InputPipe::_Read(const ValueList& args, SharedValue result)
	{
		int bufsize = -1;
		if (args.size() > 0)
		{
			bufsize = args.at(0)->ToInt();
		}
		
		AutoPtr<Blob> blob = Read(bufsize);
		result->SetObject(blob);
	}
	
	void InputPipe::_ReadLine(const ValueList& args, SharedValue result)
	{
		AutoPtr<Blob> blob = ReadLine();
		if (blob.isNull())
		{
			result->SetNull();
		}
		else
		{
			result->SetObject(blob);
		}
	}
	
	void InputPipe::_Join(const ValueList& args, SharedValue result)
	{
		if (args.size() > 0 && args.at(0)->IsObject())
		{
			AutoInputPipe pipe = args.at(0)->ToObject().cast<InputPipe>();
			if (!pipe.isNull())
			{
				this->Join(pipe);
			}
		}
	}
	
	void InputPipe::_Unjoin(const ValueList& args, SharedValue result)
	{
		if (args.size() > 0 && args.at(0)->IsObject())
		{
			AutoInputPipe pipe = args.at(0)->ToObject().cast<InputPipe>();
			if (!pipe.isNull())
			{
				this->Unjoin(pipe);
			}
		}
	}
	
	void InputPipe::_IsJoined(const ValueList& args, SharedValue result)
	{
		result->SetBool(this->IsJoined());
	}
	
	void InputPipe::_Split(const ValueList& args, SharedValue result)
	{
		this->Split();
		SharedKList list = new StaticBoundList();
		list->Append(Value::NewObject(splitPipe1));
		list->Append(Value::NewObject(splitPipe2));
		result->SetList(list);
	}
	
	void InputPipe::_Unsplit(const ValueList& args, SharedValue result)
	{
		this->Unsplit();
	}
	
	void InputPipe::_IsSplit(const ValueList& args, SharedValue result)
	{
		result->SetBool(this->IsSplit());
	}
	
	void InputPipe::_Attach(const ValueList& args, SharedValue result)
	{
		if (args.size() >= 0 && args.at(0)->IsObject())
		{
			this->Attach(args.at(0)->ToObject());
		}
	}
	
	void InputPipe::_Detach(const ValueList& args, SharedValue result)
	{
		this->Detach();
	}
	
	void InputPipe::_IsAttached(const ValueList& args, SharedValue result)
	{
		result->SetBool(this->IsAttached());
	}
	
	void InputPipe::_IsPiped(const ValueList& args, SharedValue result)
	{
		result->SetBool(this->IsPiped());
	}
	
	void InputPipe::_SetOnRead(const ValueList& args, SharedValue result)
	{
		if (args.size() >= 0 && args.at(0)->IsMethod())
		{
			SetOnRead(args.at(0)->ToMethod());
		}
	}
	
	void InputPipe::_SetOnClose(const ValueList& args, SharedValue result)
	{
		if (args.size() >= 0 && args.at(0)->IsMethod())
		{
			this->onClose = new SharedKMethod(args.at(0)->ToMethod());
		}
	}
}
